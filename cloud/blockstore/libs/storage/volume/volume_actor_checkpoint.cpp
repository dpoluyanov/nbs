#include "volume_actor.h"

#include <cloud/blockstore/libs/service/request_helpers.h>
#include <cloud/blockstore/libs/storage/api/partition.h>
#include <cloud/blockstore/libs/storage/api/undelivered.h>
#include <cloud/blockstore/libs/storage/core/probes.h>

#include <cloud/storage/core/libs/common/verify.h>
#include <cloud/storage/core/libs/diagnostics/trace_serializer.h>

namespace NCloud::NBlockStore::NStorage {

using namespace NActors;
using namespace NKikimr;
using namespace NKikimr::NTabletFlatExecutor;
using namespace NPartition;

LWTRACE_USING(BLOCKSTORE_STORAGE_PROVIDER);

namespace {

////////////////////////////////////////////////////////////////////////////////

struct TPartitionDescr
{
    ui64 TabletId = 0;
    TActorId ActorId;
    bool DiskRegistryBasedDisk = false;
};

using TPartitionDescrs = TVector<TPartitionDescr>;

////////////////////////////////////////////////////////////////////////////////

using TCreateCheckpointMethod = TEvService::TCreateCheckpointMethod;
using TDeleteCheckpointMethod = TEvService::TDeleteCheckpointMethod;
using TDeleteCheckpointDataMethod = TEvVolume::TDeleteCheckpointDataMethod;
using TGetChangedBlocksMethod = TEvService::TGetChangedBlocksMethod;

const char* GetCheckpointRequestName(ECheckpointRequestType reqType) {
    switch (reqType) {
        case ECheckpointRequestType::Delete:
            return TDeleteCheckpointMethod::Name;
        case ECheckpointRequestType::DeleteData:
            return TDeleteCheckpointDataMethod::Name;
        case ECheckpointRequestType::Create:
            return TCreateCheckpointMethod::Name;
    }
}

////////////////////////////////////////////////////////////////////////////////

template <typename TMethod>
class TCheckpointActor final
    : public TActorBootstrapped<TCheckpointActor<TMethod>>
{
private:
    using TThis = TCheckpointActor<TMethod>;
    using TBase = TActor<TThis>;

private:
    const TRequestInfoPtr RequestInfo;
    const ui64 RequestId;
    const TString CheckpointId;
    const ui64 VolumeTabletId;
    const TActorId VolumeActorId;
    const TPartitionDescrs PartitionDescrs;
    const TRequestTraceInfo TraceInfo;

    ui32 DrainResponses = 0;
    ui32 Responses = 0;
    NProto::TError Error;

    TVector<TCallContextPtr> ChildCallContexts;

public:
    TCheckpointActor(
        TRequestInfoPtr requestInfo,
        ui64 requestId,
        TString checkpointId,
        ui64 volumeTabletId,
        TActorId volumeActorId,
        TPartitionDescrs partitionDescrs,
        TRequestTraceInfo traceInfo);

    void Bootstrap(const TActorContext& ctx);
    void Drain(const TActorContext& ctx);
    void DoAction(const TActorContext& ctx);
    void UpdateCheckpointRequest(const TActorContext& ctx, bool completed);
    void ReplyAndDie(const TActorContext& ctx);

private:
    void ForkTraces(TCallContextPtr callContext);
    void JoinTraces(ui32 cookie);

private:
    STFUNC(StateDrain);
    STFUNC(StateDoAction);
    STFUNC(StateUpdateCheckpointRequest);

    void HandleResponse(
        const typename TMethod::TResponse::TPtr& ev,
        const TActorContext& ctx);

    void HandleUndelivery(
        const typename TMethod::TRequest::TPtr& ev,
        const TActorContext& ctx);

    void HandleDrainResponse(
        const TEvPartition::TEvDrainResponse::TPtr& ev,
        const TActorContext& ctx);

    void HandleDrainUndelivery(
        const TEvPartition::TEvDrainRequest::TPtr& ev,
        const TActorContext& ctx);

    void HandleUpdateCheckpointRequestResponse(
        const TEvVolumePrivate::TEvUpdateCheckpointRequestResponse::TPtr& ev,
        const TActorContext& ctx);

    void HandlePoisonPill(
        const TEvents::TEvPoisonPill::TPtr& ev,
        const TActorContext& ctx);
};

////////////////////////////////////////////////////////////////////////////////

template <typename TMethod>
TCheckpointActor<TMethod>::TCheckpointActor(
        TRequestInfoPtr requestInfo,
        ui64 requestId,
        TString checkpointId,
        ui64 volumeTabletId,
        TActorId volumeActorId,
        TPartitionDescrs partitionDescrs,
        TRequestTraceInfo traceInfo)
    : RequestInfo(std::move(requestInfo))
    , RequestId(requestId)
    , CheckpointId(std::move(checkpointId))
    , VolumeTabletId(volumeTabletId)
    , VolumeActorId(volumeActorId)
    , PartitionDescrs(std::move(partitionDescrs))
    , TraceInfo(std::move(traceInfo))
    , ChildCallContexts(Reserve(PartitionDescrs.size()))
{
    TBase::ActivityType = TBlockStoreActivities::VOLUME;
}

template <typename TMethod>
void TCheckpointActor<TMethod>::Drain(const TActorContext& ctx)
{
    ui32 cookie = 0;
    for (const auto& x: PartitionDescrs) {
        LOG_DEBUG(ctx, TBlockStoreComponents::VOLUME,
            "[%lu] Sending Drain request to partition %lu",
            VolumeTabletId,
            x.TabletId);

        const auto selfId = TBase::SelfId();
        auto request = std::make_unique<TEvPartition::TEvDrainRequest>();

        ForkTraces(request->CallContext);

        auto event = std::make_unique<IEventHandle>(
            x.ActorId,
            selfId,
            request.release(),
            IEventHandle::FlagForwardOnNondelivery,
            cookie++,
            &selfId
        );

        ctx.Send(event.release());
    }

    TBase::Become(&TThis::StateDrain);
}

template <typename TMethod>
void TCheckpointActor<TMethod>::UpdateCheckpointRequest(
    const TActorContext& ctx,
    bool completed)
{
    LOG_DEBUG(ctx, TBlockStoreComponents::VOLUME,
        "[%lu] Sending UpdateCheckpointRequest to volume",
        VolumeTabletId);

    const auto selfId = TBase::SelfId();

    auto requestInfo = CreateRequestInfo(
        selfId,
        0,  // cookie
        RequestInfo->CallContext
    );

    auto request =
        std::make_unique<TEvVolumePrivate::TEvUpdateCheckpointRequestRequest>(
            std::move(requestInfo),
            RequestId,
            completed);

    auto event = std::make_unique<IEventHandle>(
        VolumeActorId,
        selfId,
        request.release(),
        IEventHandle::FlagForwardOnNondelivery,
        0,  // cookie
        &selfId
    );

    ctx.Send(event.release());

    TBase::Become(&TThis::StateUpdateCheckpointRequest);
}

template <typename TMethod>
void TCheckpointActor<TMethod>::ReplyAndDie(const TActorContext& ctx)
{
    auto response = std::make_unique<typename TMethod::TResponse>(Error);

    if (TraceInfo.IsTraced) {
        TraceInfo.TraceSerializer->BuildTraceInfo(
            *response->Record.MutableTrace(),
            RequestInfo->CallContext->LWOrbit,
            TraceInfo.ReceiveTime,
            GetCycleCount());
    }

    LWTRACK(
        ResponseSent_Volume,
        RequestInfo->CallContext->LWOrbit,
        TMethod::Name,
        RequestInfo->CallContext->RequestId);

    NCloud::Reply(ctx, *RequestInfo, std::move(response));

    TBase::Die(ctx);
}

template <typename TMethod>
void TCheckpointActor<TMethod>::ForkTraces(TCallContextPtr callContext)
{
    auto& cc = RequestInfo->CallContext;
    if (!cc->LWOrbit.Fork(callContext->LWOrbit)) {
        LWTRACK(ForkFailed, cc->LWOrbit, TMethod::Name, cc->RequestId);
    }

    ChildCallContexts.push_back(callContext);
}

template <typename TMethod>
void TCheckpointActor<TMethod>::JoinTraces(ui32 cookie)
{
    if (cookie < ChildCallContexts.size()) {
        if (ChildCallContexts[cookie]) {
            auto& cc = RequestInfo->CallContext;
            cc->LWOrbit.Join(ChildCallContexts[cookie]->LWOrbit);
            ChildCallContexts[cookie].Reset();
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

template <typename TMethod>
void TCheckpointActor<TMethod>::HandleDrainResponse(
    const TEvPartition::TEvDrainResponse::TPtr& ev,
    const TActorContext& ctx)
{
    auto* msg = ev->Get();

    JoinTraces(ev->Cookie);

    if (FAILED(msg->GetStatus())) {
        Error = msg->GetError();

        NCloud::Send(
            ctx,
            VolumeActorId,
            std::make_unique<TEvents::TEvPoisonPill>()
        );

        ReplyAndDie(ctx);
        return;
    }

    if (++DrainResponses == PartitionDescrs.size()) {
        ChildCallContexts.clear();
        DoAction(ctx);
    }
}

template <typename TMethod>
void TCheckpointActor<TMethod>::HandleDrainUndelivery(
    const TEvPartition::TEvDrainRequest::TPtr& ev,
    const TActorContext& ctx)
{
    Y_UNUSED(ev);

    Error = MakeError(
        E_REJECTED,
        "failed to deliver drain request to some partitions"
    );

    NCloud::Send(
        ctx,
        VolumeActorId,
        std::make_unique<TEvents::TEvPoisonPill>()
    );

    ReplyAndDie(ctx);
}

////////////////////////////////////////////////////////////////////////////////

template <typename TMethod>
void TCheckpointActor<TMethod>::HandleResponse(
    const typename TMethod::TResponse::TPtr& ev,
    const TActorContext& ctx)
{
    auto* msg = ev->Get();

    JoinTraces(ev->Cookie);

    if (FAILED(msg->GetStatus())) {
        Error = msg->GetError();

        NCloud::Send(
            ctx,
            VolumeActorId,
            std::make_unique<TEvents::TEvPoisonPill>()
        );

        ReplyAndDie(ctx);
        return;
    }

    if (++Responses == PartitionDescrs.size()) {
        // All the underlying actors have sent a successful status, we mark the
        // request as completed.
        UpdateCheckpointRequest(ctx, true);
    }
}

template <typename TMethod>
void TCheckpointActor<TMethod>::HandleUndelivery(
    const typename TMethod::TRequest::TPtr& ev,
    const TActorContext& ctx)
{
    Y_UNUSED(ev);

    Error = MakeError(
        E_REJECTED,
        "failed to deliver checkpoint request to some partitions"
    );

    NCloud::Send(
        ctx,
        VolumeActorId,
        std::make_unique<TEvents::TEvPoisonPill>()
    );

    ReplyAndDie(ctx);
}

////////////////////////////////////////////////////////////////////////////////

template <typename TMethod>
void TCheckpointActor<TMethod>::HandleUpdateCheckpointRequestResponse(
    const TEvVolumePrivate::TEvUpdateCheckpointRequestResponse::TPtr& ev,
    const TActorContext& ctx)
{
    Y_UNUSED(ev);

    ReplyAndDie(ctx);
}

template <typename TMethod>
void TCheckpointActor<TMethod>::HandlePoisonPill(
    const TEvents::TEvPoisonPill::TPtr& ev,
    const TActorContext& ctx)
{
    Y_UNUSED(ev);

    Error = MakeError(
        E_REJECTED,
        "tablet is dead"
    );

    ReplyAndDie(ctx);
}

////////////////////////////////////////////////////////////////////////////////

template <typename TMethod>
STFUNC(TCheckpointActor<TMethod>::StateDrain)
{
    switch (ev->GetTypeRewrite()) {
        HFunc(TEvPartition::TEvDrainResponse, HandleDrainResponse);
        HFunc(TEvPartition::TEvDrainRequest, HandleDrainUndelivery);
        HFunc(TEvents::TEvPoisonPill, HandlePoisonPill);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::VOLUME);
            break;
    }
}

template <typename TMethod>
STFUNC(TCheckpointActor<TMethod>::StateDoAction)
{
    switch (ev->GetTypeRewrite()) {
        HFunc(TMethod::TResponse, HandleResponse);
        HFunc(TMethod::TRequest, HandleUndelivery);
        HFunc(TEvents::TEvPoisonPill, HandlePoisonPill);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::VOLUME);
            break;
    }
}

template <typename TMethod>
STFUNC(TCheckpointActor<TMethod>::StateUpdateCheckpointRequest)
{
    switch (ev->GetTypeRewrite()) {
        HFunc(
            TEvVolumePrivate::TEvUpdateCheckpointRequestResponse,
            HandleUpdateCheckpointRequestResponse
        );
        HFunc(TEvents::TEvPoisonPill, HandlePoisonPill);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::VOLUME);
            break;
    }
}

////////////////////////////////////////////////////////////////////////////////

template <typename TMethod>
void TCheckpointActor<TMethod>::DoAction(const TActorContext& ctx)
{
    ui32 cookie = 0;
    for (const auto& x: PartitionDescrs) {
        if (x.DiskRegistryBasedDisk) {
            Y_VERIFY_DEBUG(PartitionDescrs.size() == 1);
            // It is not required to send requests to the actor, we reject
            // zero/write requests in TVolume (see CanExecuteWriteRequest).

            // Here we make a decision about the success of the task.
            bool okToExecute = true;
            if constexpr (std::is_same_v<TMethod, TCreateCheckpointMethod>) {
                if (!CheckpointId) {
                    okToExecute = false;
                    Error = MakeError(E_ARGUMENT, "empty checkpoint name");
                }
            }
            UpdateCheckpointRequest(ctx, okToExecute);
            return;
        }

        LOG_DEBUG(ctx, TBlockStoreComponents::VOLUME,
            "[%lu] Sending %s request to partition %lu",
            VolumeTabletId,
            TMethod::Name,
            x.TabletId);

        const auto selfId = TBase::SelfId();
        auto request = std::make_unique<typename TMethod::TRequest>();
        request->Record.SetCheckpointId(CheckpointId);

        ForkTraces(request->CallContext);

        auto event = std::make_unique<IEventHandle>(
            x.ActorId,
            selfId,
            request.release(),
            IEventHandle::FlagForwardOnNondelivery,
            cookie++,
            &selfId
        );

        ctx.Send(event.release());
    }

    TBase::Become(&TThis::StateDoAction);
}

template <typename TMethod>
void TCheckpointActor<TMethod>::Bootstrap(const TActorContext& ctx)
{
    DoAction(ctx);
}

template <>
void TCheckpointActor<TCreateCheckpointMethod>::Bootstrap(
    const TActorContext& ctx)
{
    // if this volume is a single-partition blobstorage-based disk, draining is
    // not needed.
    const bool skipDrain = (PartitionDescrs.size() == 1) &&
                           (PartitionDescrs[0].DiskRegistryBasedDisk == false);

    if (skipDrain) {
        DoAction(ctx);
    } else {
        Drain(ctx);
    }
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

void TVolumeActor::CreateCheckpointLightRequest(
    const TActorContext& ctx,
    ui64 requestId,
    const TString& checkpointId)
{
    LOG_INFO(ctx, TBlockStoreComponents::VOLUME,
        "[%lu] %s created light checkpoint %s",
        TabletID(),
        TCreateCheckpointMethod::Name,
        checkpointId.Quote().c_str());

    TCheckpointLight& checkpointLight = State->StartCheckpointLight();
    checkpointLight.CreateCheckpoint(checkpointId);

    const TCheckpointRequestInfo* requestInfo =
        CheckpointRequests.FindPtr(requestId);

    ExecuteTx<TUpdateCheckpointRequest>(
        ctx,
        requestInfo ? std::move(requestInfo->RequestInfo) : nullptr,
        requestId,
        true // Completed
    );
}


void TVolumeActor::DeleteCheckpointLightRequest(
    const TActorContext& ctx,
    ui64 requestId,
    const TString& checkpointId)
{
    TCheckpointLight* checkpointLight = State->GetCheckpointLight();
    if (checkpointLight &&
        checkpointLight->GetCheckpointId() == checkpointId)
    {
        State->StopCheckpointLight();
    }

    const TCheckpointRequestInfo* requestInfo =
        CheckpointRequests.FindPtr(requestId);

    ExecuteTx<TUpdateCheckpointRequest>(
        ctx,
        requestInfo ? std::move(requestInfo->RequestInfo) : nullptr,
        requestId,
        true // Completed
    );
}

bool TVolumeActor::HandleGetChangedBlocksLightRequest(
    const TGetChangedBlocksMethod::TRequest::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();

    auto response = std::make_unique<TGetChangedBlocksMethod::TResponse>();
    auto requestInfo =
            CreateRequestInfo(ev->Sender, ev->Cookie, msg->CallContext);
    auto checkpointLight = State->GetCheckpointLight();

    if (!checkpointLight) {
        LOG_ERROR(ctx, TBlockStoreComponents::VOLUME,
            "[%lu] %s Light checkpoint disabled",
            TabletID(),
            TGetChangedBlocksMethod::Name);

        *response->Record.MutableError() =
            MakeError(E_REJECTED, "Light checkpoint disabled");
    } else {
        const ui64 beginBlock =
            msg->Record.GetStartIndex();
        const ui64 endBlock =
            beginBlock + msg->Record.GetBlocksCount();

        if (endBlock > State->GetBlocksCount()) {
            LOG_ERROR(ctx, TBlockStoreComponents::VOLUME,
                "[%lu] %s Invalid blocks requested %d blocks %d",
                TabletID(),
                TGetChangedBlocksMethod::Name,
                endBlock,
                State->GetBlocksCount());

            *response->Record.MutableError() =
                MakeError(E_REJECTED, "Invalid blocks requested");
        } else {
            const auto& data =
                checkpointLight->GetCheckpointData();

            response->Record.MutableMask()->reserve(
                msg->Record.GetBlocksCount() / 8 + 1);
            for (auto i = beginBlock; i < endBlock;) {
                ui8 bitData = 0;
                for (auto j = 0; j < 8 && i < endBlock; ++j) {
                    bitData |= data.Test(i) << j;
                    ++i;
                }
                response->Record.MutableMask()->push_back(bitData);
            }
        }
    }

    NCloud::Reply(ctx, *requestInfo, std::move(response));
    return true;
}

template <>
bool TVolumeActor::HandleRequest<TCreateCheckpointMethod>(
    const TActorContext& ctx,
    const TCreateCheckpointMethod::TRequest::TPtr& ev,
    ui64 volumeRequestId,
    bool isTraced,
    ui64 traceTs)
{
    Y_UNUSED(volumeRequestId);

    auto requestInfo = CreateRequestInfo<TCreateCheckpointMethod>(
        ev->Sender,
        ev->Cookie,
        ev->Get()->CallContext);

    AddTransaction(*requestInfo);

    const auto& checkpointRequest = State->GetCheckpointStore().CreateNew(
        ev->Get()->Record.GetCheckpointId(),
        ctx.Now(),
        ECheckpointRequestType::Create,
        ev->Get()->Record.GetIsLight()
            ? ECheckpointType::Light
            : ECheckpointType::Normal);
    ExecuteTx<TSaveCheckpointRequest>(
        ctx,
        std::move(requestInfo),
        checkpointRequest.RequestId,
        isTraced,
        traceTs);

    return true;
}

template <>
bool TVolumeActor::HandleRequest<TDeleteCheckpointMethod>(
    const TActorContext& ctx,
    const TDeleteCheckpointMethod::TRequest::TPtr& ev,
    ui64 volumeRequestId,
    bool isTraced,
    ui64 traceTs)
{
     Y_UNUSED(volumeRequestId);

    auto requestInfo = CreateRequestInfo<TDeleteCheckpointMethod>(
        ev->Sender,
        ev->Cookie,
        ev->Get()->CallContext);

    AddTransaction(*requestInfo);

    auto& checkpointStore = State->GetCheckpointStore();
    const auto& checkpointId = ev->Get()->Record.GetCheckpointId();
    const auto type = checkpointStore.GetCheckpointType(checkpointId);

    const auto& checkpointRequest = checkpointStore.CreateNew(
        checkpointId,
        ctx.Now(),
        ECheckpointRequestType::Delete,
        type.value_or(ECheckpointType::Normal));
    ExecuteTx<TSaveCheckpointRequest>(
        ctx,
        std::move(requestInfo),
        checkpointRequest.RequestId,
        isTraced,
        traceTs);

    return true;
}

template <>
bool TVolumeActor::HandleRequest<TDeleteCheckpointDataMethod>(
    const TActorContext& ctx,
    const TDeleteCheckpointDataMethod::TRequest::TPtr& ev,
    ui64 volumeRequestId,
    bool isTraced,
    ui64 traceTs)
{
    Y_UNUSED(volumeRequestId);

    auto requestInfo = CreateRequestInfo<TDeleteCheckpointDataMethod>(
        ev->Sender,
        ev->Cookie,
        ev->Get()->CallContext);

    AddTransaction(*requestInfo);

    const auto& checkpointRequest = State->GetCheckpointStore().CreateNew(
        ev->Get()->Record.GetCheckpointId(),
        ctx.Now(),
        ECheckpointRequestType::DeleteData,
        ECheckpointType::Normal);
    ExecuteTx<TSaveCheckpointRequest>(
        ctx,
        std::move(requestInfo),
        checkpointRequest.RequestId,
        isTraced,
        traceTs);

    return true;
}

////////////////////////////////////////////////////////////////////////////////

void TVolumeActor::HandleUpdateCheckpointRequest(
    const TEvVolumePrivate::TEvUpdateCheckpointRequestRequest::TPtr& ev,
    const TActorContext& ctx)
{
    auto* msg = ev->Get();

    ExecuteTx<TUpdateCheckpointRequest>(
        ctx,
        std::move(msg->RequestInfo),
        msg->RequestId,
        msg->Completed
    );
}

////////////////////////////////////////////////////////////////////////////////

void TVolumeActor::ProcessNextCheckpointRequest(const TActorContext& ctx)
{
    // only one simultaneous checkpoint request is supported, other requests
    // should wait
    if (State->GetCheckpointStore().IsRequestInProgress()) {
        return;
    }

    // there is no FIFO guarantee for requests sent via TPartitionRequestActor
    // and requests forwarded via TVolumeActor => we can start checkpoint
    // creation only if there are no TPartitionRequestActor-based requests
    // in flight currently
    if (MultipartitionWriteAndZeroRequestsInProgress) {
        return;
    }

    ui64 readyToExecuteRequestId = 0;
    // nothing to do
    if (!State->GetCheckpointStore().HasRequestToExecute(
            &readyToExecuteRequestId))
    {
        return;
    }

    const TCheckpointRequest& request =
        State->GetCheckpointStore().GetRequestById(readyToExecuteRequestId);

    TCheckpointRequestInfo emptyRequestInfo(
        CreateRequestInfo(
            SelfId(),
            0,   // cookie
            MakeIntrusive<TCallContext>()),
        false,
        GetCycleCount());
    const TCheckpointRequestInfo* requestInfo =
        CheckpointRequests.FindPtr(readyToExecuteRequestId);
    if (!requestInfo) {
        requestInfo = &emptyRequestInfo;
    }

    TPartitionDescrs partitionDescrs;
    for (const auto& p: State->GetPartitions()) {
        partitionDescrs.push_back({p.TabletId, p.Owner, false});

        LOG_TRACE(ctx, TBlockStoreComponents::VOLUME,
            "[%lu] Forward %s request to partition: %lu %s",
            TabletID(),
            GetCheckpointRequestName(request.ReqType),
            p.TabletId,
            ToString(p.Owner).data()
        );
    }

    if (auto actorId = State->GetNonreplicatedPartitionActor()) {
        ui64 TabletId = 0;
        partitionDescrs.push_back({TabletId, actorId, true});

        LOG_TRACE(
            ctx,
            TBlockStoreComponents::VOLUME,
            "[%lu] Forward %s request to nonrepl partition: %s",
            TabletID(),
            GetCheckpointRequestName(request.ReqType),
            ToString(actorId).c_str());
    }

    State->GetCheckpointStore().SetCheckpointRequestInProgress(
        readyToExecuteRequestId);

    TActorId actorId;
    switch (request.ReqType) {
        case ECheckpointRequestType::Create: {
            if (request.Type == ECheckpointType::Light) {
                return CreateCheckpointLightRequest(
                    ctx,
                    request.RequestId,
                    request.CheckpointId);
            }
            actorId =
                NCloud::Register<TCheckpointActor<TCreateCheckpointMethod>>(
                    ctx,
                    requestInfo->RequestInfo,
                    request.RequestId,
                    request.CheckpointId,
                    TabletID(),
                    SelfId(),
                    std::move(partitionDescrs),
                    TRequestTraceInfo(
                        requestInfo->IsTraced,
                        requestInfo->TraceTs,
                        TraceSerializer));
            break;
        }
        case ECheckpointRequestType::Delete: {
            if (request.Type == ECheckpointType::Light) {
                return DeleteCheckpointLightRequest(
                    ctx,
                    request.RequestId,
                    request.CheckpointId);
            }
            actorId =
                NCloud::Register<TCheckpointActor<TDeleteCheckpointMethod>>(
                    ctx,
                    requestInfo->RequestInfo,
                    request.RequestId,
                    request.CheckpointId,
                    TabletID(),
                    SelfId(),
                    std::move(partitionDescrs),
                    TRequestTraceInfo(
                        requestInfo->IsTraced,
                        requestInfo->TraceTs,
                        TraceSerializer));
            break;
        }
        case ECheckpointRequestType::DeleteData: {
            actorId =
                NCloud::Register<TCheckpointActor<TDeleteCheckpointDataMethod>>(
                    ctx,
                    requestInfo->RequestInfo,
                    request.RequestId,
                    request.CheckpointId,
                    TabletID(),
                    SelfId(),
                    std::move(partitionDescrs),
                    TRequestTraceInfo(
                        requestInfo->IsTraced,
                        requestInfo->TraceTs,
                        TraceSerializer));
            break;
        }
        default:
            Y_FAIL();
    }

    Actors.insert(actorId);
}

////////////////////////////////////////////////////////////////////////////////

bool TVolumeActor::PrepareSaveCheckpointRequest(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxVolume::TSaveCheckpointRequest& args)
{
    Y_UNUSED(ctx);
    Y_UNUSED(tx);
    Y_UNUSED(args);

    return true;
}

void TVolumeActor::ExecuteSaveCheckpointRequest(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxVolume::TSaveCheckpointRequest& args)
{
    Y_UNUSED(ctx);

    TVolumeDatabase db(tx.DB);

    // The request in memory has the Received status.
    // We save this request to the database with the Saved status.
    // After receiving confirmation of the transaction,
    //  we will raise the status in memory to Saved too.
    auto checkpointRequestCopy =
        State->GetCheckpointStore().GetRequestById(args.RequestId);
    Y_VERIFY_DEBUG(
        checkpointRequestCopy.State == ECheckpointRequestState::Received);
    checkpointRequestCopy.State = ECheckpointRequestState::Saved;

    db.WriteCheckpointRequest(checkpointRequestCopy);
}

void TVolumeActor::CompleteSaveCheckpointRequest(
    const TActorContext& ctx,
    TTxVolume::TSaveCheckpointRequest& args)
{
    const auto& checkpointRequest =
        State->GetCheckpointStore().GetRequestById(args.RequestId);

    LOG_DEBUG(ctx, TBlockStoreComponents::VOLUME,
        "[%lu] CheckpointRequest %lu %s saved",
        TabletID(),
        checkpointRequest.RequestId,
        checkpointRequest.CheckpointId.Quote().c_str());

    State->GetCheckpointStore().SetCheckpointRequestSaved(args.RequestId);

    CheckpointRequests.insert(
        {args.RequestId, {args.RequestInfo, args.IsTraced, args.TraceTs}});

    RemoveTransaction(*args.RequestInfo);
    ProcessNextCheckpointRequest(ctx);
}

////////////////////////////////////////////////////////////////////////////////

bool TVolumeActor::CanExecuteWriteRequest() const
{
    const bool doesCheckpointExist =
        State->GetCheckpointStore().DoesCheckpointWithDataExist();
    const bool isCheckpointBeingCreated =
        State->GetCheckpointStore().IsCheckpointBeingCreated();

    const bool isSinglePartition = State->GetPartitions().size() < 2;

    // If our volume is DiskRegistry-based, write requests are allowed only if
    // there are no active checkpoints
    if (State->GetNonreplicatedPartitionActor()) {
        const bool checkpointExistsOrCreatingNow =
            doesCheckpointExist || isCheckpointBeingCreated;

        return !checkpointExistsOrCreatingNow;
    }

    // If our volume is BlobStorage-based, write requests are allowed only if we
    // have only one partition or there is no checkpoint creation request in
    // flight
    return isSinglePartition || !isCheckpointBeingCreated;
}

bool TVolumeActor::PrepareUpdateCheckpointRequest(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxVolume::TUpdateCheckpointRequest& args)
{
    Y_UNUSED(ctx);
    Y_UNUSED(tx);
    Y_UNUSED(args);

    return true;
}

void TVolumeActor::ExecuteUpdateCheckpointRequest(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxVolume::TUpdateCheckpointRequest& args)
{
    Y_UNUSED(ctx);

    TVolumeDatabase db(tx.DB);
    db.UpdateCheckpointRequest(args.RequestId, args.Completed);
}

void TVolumeActor::CompleteUpdateCheckpointRequest(
    const TActorContext& ctx,
    TTxVolume::TUpdateCheckpointRequest& args)
{
    const TCheckpointRequest& request =
        State->GetCheckpointStore().GetRequestById(args.RequestId);

    LOG_DEBUG(ctx, TBlockStoreComponents::VOLUME,
        "[%lu] CheckpointRequest %lu %s marked %s",
        TabletID(),
        request.RequestId,
        request.CheckpointId.Quote().c_str(),
        args.Completed ? "completed" : "rejected");

    State->GetCheckpointStore().SetCheckpointRequestFinished(
        request.RequestId,
        args.Completed);
    CheckpointRequests.erase(request.RequestId);

    if (request.Type == ECheckpointType::Light) {
        const bool needReply =
            args.RequestInfo && SelfId() != args.RequestInfo->Sender;
        if (needReply) {
            if (request.ReqType == ECheckpointRequestType::Create) {
                NCloud::Reply(
                    ctx,
                    *args.RequestInfo,
                    std::make_unique<TCreateCheckpointMethod::TResponse>());
            } else if (request.ReqType == ECheckpointRequestType::Delete) {
                NCloud::Reply(
                    ctx,
                    *args.RequestInfo,
                    std::make_unique<TDeleteCheckpointMethod::TResponse>());
            }
        }
    } else {
        NCloud::Reply(
            ctx,
            *args.RequestInfo,
            std::make_unique<TEvVolumePrivate::TEvUpdateCheckpointRequestResponse>()
        );
        Actors.erase(args.RequestInfo->Sender);
    }

    ProcessNextCheckpointRequest(ctx);
}

}   // namespace NCloud::NBlockStore::NStorage
