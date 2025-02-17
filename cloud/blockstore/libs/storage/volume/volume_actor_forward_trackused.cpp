#include "volume_actor.h"

#include "actors/forward_read.h"
#include "actors/forward_write_and_mark_used.h"

#include <cloud/blockstore/libs/service/request_helpers.h>
#include <cloud/blockstore/libs/storage/api/undelivered.h>
#include <cloud/blockstore/libs/storage/core/forward_helpers.h>

#include <cloud/storage/core/libs/common/media.h>

#include <cloud/blockstore/libs/storage/core/probes.h>

namespace NCloud::NBlockStore::NStorage {

using namespace NActors;

LWTRACE_USING(BLOCKSTORE_STORAGE_PROVIDER);

////////////////////////////////////////////////////////////////////////////////

template <typename TMethod>
bool TVolumeActor::SendRequestToPartitionWithUsedBlockTracking(
    const TActorContext& ctx,
    const typename TMethod::TRequest::TPtr& ev,
    const TActorId& partActorId,
    const ui64 volumeRequestId)
{
    const auto* msg = ev->Get();

    const auto& volumeConfig = State->GetMeta().GetVolumeConfig();
    auto encryptedNonreplicatedVolume =
        IsDiskRegistryMediaKind(State->GetConfig().GetStorageMediaKind()) &&
        volumeConfig.GetEncryptionDesc().GetMode() != NProto::NO_ENCRYPTION;

    if constexpr (IsWriteMethod<TMethod>) {
        if (State->GetTrackUsedBlocks() || State->GetCheckpointLight()) {
            auto requestInfo =
                CreateRequestInfo(ev->Sender, ev->Cookie, msg->CallContext);

            NCloud::Register<TWriteAndMarkUsedActor<TMethod>>(
                ctx,
                std::move(requestInfo),
                std::move(msg->Record),
                State->GetBlockSize(),
                encryptedNonreplicatedVolume,
                volumeRequestId,
                partActorId,
                TabletID(),
                SelfId());

            return true;
        }
    }

    if constexpr (IsReadMethod<TMethod>) {
        if (State->GetMaskUnusedBlocks() && State->GetUsedBlocks() ||
            encryptedNonreplicatedVolume)
        {
            THashSet<ui64> unusedIndices;

            FillUnusedIndices(
                msg->Record,
                State->GetUsedBlocks(),
                &unusedIndices);

            if (unusedIndices) {
                auto requestInfo =
                    CreateRequestInfo(ev->Sender, ev->Cookie, msg->CallContext);

                NCloud::Register<TReadActor<TMethod>>(
                    ctx,
                    std::move(requestInfo),
                    std::move(msg->Record),
                    std::move(unusedIndices),
                    State->GetMaskUnusedBlocks(),
                    encryptedNonreplicatedVolume,
                    partActorId,
                    TabletID(),
                    SelfId());

                return true;
            }
        }
    }

    if constexpr (std::is_same_v<TMethod, TEvService::TGetChangedBlocksMethod>) {
        const auto type = State->GetCheckpointStore().GetCheckpointType(
            msg->Record.GetHighCheckpointId());
        if (type.value_or(ECheckpointType::Normal) == ECheckpointType::Light){
            return HandleGetChangedBlocksLightRequest(ev, ctx);
        }
    }

    return false;
}

////////////////////////////////////////////////////////////////////////////////

#define GENERATE_IMPL(name, ns)                                                \
template bool TVolumeActor::SendRequestToPartitionWithUsedBlockTracking<       \
    ns::T##name##Method>(                                                      \
        const TActorContext& ctx,                                              \
        const ns::TEv##name##Request::TPtr& ev,                                \
        const TActorId& partActorId,                                           \
        const ui64 volumeRequestId);                                           \
// GENERATE_IMPL

GENERATE_IMPL(ReadBlocks,         TEvService)
GENERATE_IMPL(WriteBlocks,        TEvService)
GENERATE_IMPL(ZeroBlocks,         TEvService)
GENERATE_IMPL(CreateCheckpoint,   TEvService)
GENERATE_IMPL(DeleteCheckpoint,   TEvService)
GENERATE_IMPL(GetChangedBlocks,   TEvService)
GENERATE_IMPL(ReadBlocksLocal,    TEvService)
GENERATE_IMPL(WriteBlocksLocal,   TEvService)

GENERATE_IMPL(DescribeBlocks,           TEvVolume)
GENERATE_IMPL(GetUsedBlocks,            TEvVolume)
GENERATE_IMPL(GetPartitionInfo,         TEvVolume)
GENERATE_IMPL(CompactRange,             TEvVolume)
GENERATE_IMPL(GetCompactionStatus,      TEvVolume)
GENERATE_IMPL(DeleteCheckpointData,     TEvVolume)
GENERATE_IMPL(RebuildMetadata,          TEvVolume)
GENERATE_IMPL(GetRebuildMetadataStatus, TEvVolume)
GENERATE_IMPL(ScanDisk,                 TEvVolume)
GENERATE_IMPL(GetScanDiskStatus,        TEvVolume)

#undef GENERATE_IMPL

}   // namespace NCloud::NBlockStore::NStorage
