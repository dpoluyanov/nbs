#include "hive_proxy_actor.h"

#include <ydb/core/mind/local.h>

namespace NCloud::NStorage {

using namespace NActors;

using namespace NKikimr;

namespace {

////////////////////////////////////////////////////////////////////////////////

class TBootRequestActor final
    : public TActorBootstrapped<TBootRequestActor>
{
private:
    const TActorId Owner;
    const int LogComponent;
    const THiveProxyActor::TRequestInfo Request;
    const ui64 TabletId;
    TActorId ClientId;
    TActorId TabletBootInfoCache;
    TDuration RequestTimeout;
    ui64 TimerGeneration = 0;

public:
    TBootRequestActor(
            const TActorId& owner,
            int logComponent,
            THiveProxyActor::TRequestInfo request,
            ui64 tabletId,
            TActorId clientId,
            TActorId tabletBootInfoCache,
            TDuration externalBootRequestTimeout)
        : Owner(owner)
        , LogComponent(logComponent)
        , Request(request)
        , TabletId(tabletId)
        , ClientId(clientId)
        , TabletBootInfoCache(std::move(tabletBootInfoCache))
        , RequestTimeout(externalBootRequestTimeout)
    {}

    void Bootstrap(const TActorContext& ctx);

private:
    template<class... TArgs>
    void ReplyAndDie(const TActorContext& ctx, TArgs&&... args);

    void HandleChangeTabletClient(
        const TEvHiveProxyPrivate::TEvChangeTabletClient::TPtr& ev,
        const TActorContext& ctx);

    void HandleBoot(
        const NKikimr::TEvLocal::TEvBootTablet::TPtr& ev,
        const TActorContext& ctx);

    void HandleError(
        const NKikimr::TEvHive::TEvBootTabletReply::TPtr& ev,
        const TActorContext& ctx);

    void HandleWakeup(
        const TEvents::TEvWakeup::TPtr& ev,
        const TActorContext& ctx);

    STFUNC(StateWork);
};

////////////////////////////////////////////////////////////////////////////////

void TBootRequestActor::Bootstrap(const TActorContext& ctx)
{
    NKikimr::NTabletPipe::SendData(
        ctx,
        ClientId,
        new NKikimr::TEvHive::TEvInitiateTabletExternalBoot(TabletId));

    if (RequestTimeout) {
        ctx.Schedule(RequestTimeout, new TEvents::TEvWakeup(++TimerGeneration));
    }

    Become(&TThis::StateWork);
}

template<class... TArgs>
void TBootRequestActor::ReplyAndDie(const TActorContext& ctx, TArgs&&... args)
{
    auto response = std::make_unique<TEvHiveProxy::TEvBootExternalResponse>(
        std::forward<TArgs>(args)...);
    NCloud::Reply(ctx, Request, std::move(response));
    NCloud::Send<TEvHiveProxyPrivate::TEvRequestFinished>(
        ctx, Owner, TabletId, TabletId);
    Die(ctx);
}

void TBootRequestActor::HandleChangeTabletClient(
    const TEvHiveProxyPrivate::TEvChangeTabletClient::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();

    ClientId = msg->ClientId;

    LOG_ERROR(
        ctx,
        LogComponent,
        "Pipe to Hive has been reset. New client: %s", ToString(ClientId).c_str());

    Bootstrap(ctx);
}

void TBootRequestActor::HandleBoot(
    const NKikimr::TEvLocal::TEvBootTablet::TPtr& ev,
    const TActorContext& ctx)
{
    using EBootMode = TEvHiveProxy::TEvBootExternalResponse::EBootMode;

    const auto* msg = ev->Get();

    TTabletStorageInfoPtr storageInfo =
        TabletStorageInfoFromProto(msg->Record.GetInfo());
    ui64 suggestedGeneration = msg->Record.GetSuggestedGeneration();

    if (TabletBootInfoCache) {
        auto updateCacheRequest =
            std::make_unique<TEvHiveProxyPrivate::TEvUpdateTabletBootInfoCacheRequest>(
                storageInfo,
                suggestedGeneration
            );
        NCloud::Send(ctx, TabletBootInfoCache, std::move(updateCacheRequest));
    }

    EBootMode bootMode;
    switch (msg->Record.GetBootMode()) {
        case NKikimrLocal::BOOT_MODE_LEADER:
            bootMode = EBootMode::MASTER;
            break;
        case NKikimrLocal::BOOT_MODE_FOLLOWER:
            bootMode = EBootMode::SLAVE;
            break;
        default:
            LOG_ERROR(ctx, LogComponent,
                "Received unexpected BootMode=%u from hive",
                msg->Record.GetBootMode());
            bootMode = EBootMode::MASTER;
            break;
    }

    ReplyAndDie(
        ctx,
        std::move(storageInfo),
        suggestedGeneration,
        bootMode,
        msg->Record.GetFollowerId());
}

void TBootRequestActor::HandleError(
    const NKikimr::TEvHive::TEvBootTabletReply::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();

    ReplyAndDie(
        ctx,
        MakeKikimrError(msg->Record.GetStatus(), "External boot failed"));
}

void TBootRequestActor::HandleWakeup(
    const TEvents::TEvWakeup::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();

    if (msg->Tag != TimerGeneration) {
        return;
    }

    LOG_ERROR(ctx, LogComponent,
        "External boot request has timed out");

    ReplyAndDie(
        ctx,
        MakeKikimrError(
            NKikimrProto::EReplyStatus::TRYLATER,
            "External boot timed out"));
}

////////////////////////////////////////////////////////////////////////////////

STFUNC(TBootRequestActor::StateWork)
{
    switch (ev->GetTypeRewrite()) {
        HFunc(TEvHiveProxyPrivate::TEvChangeTabletClient, HandleChangeTabletClient);

        HFunc(NKikimr::TEvLocal::TEvBootTablet, HandleBoot);
        HFunc(NKikimr::TEvHive::TEvBootTabletReply, HandleError);

        HFunc(TEvents::TEvWakeup, HandleWakeup);

        default:
            HandleUnexpectedEvent(ev, LogComponent);
            break;
    }
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

void THiveProxyActor::HandleBootExternal(
    const TEvHiveProxy::TEvBootExternalRequest::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();

    ui64 tabletId = msg->TabletId;
    ui64 hive = GetHive(ctx, tabletId);

    auto clientId = ClientCache->Prepare(ctx, hive);
    auto requestId = NCloud::Register<TBootRequestActor>(
        ctx,
        SelfId(),
        LogComponent,
        TRequestInfo(ev->Sender, ev->Cookie),
        tabletId,
        clientId,
        TabletBootInfoCache,
        msg->RequestTimeout
    );
    HiveStates[hive].Actors.insert(requestId);
}

}   // namespace NCloud::NStorage
