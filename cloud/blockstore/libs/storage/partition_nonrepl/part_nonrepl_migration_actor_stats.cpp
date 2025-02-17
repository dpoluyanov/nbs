#include "part_nonrepl_migration_actor.h"

#include <cloud/blockstore/libs/storage/api/volume.h>

namespace NCloud::NBlockStore::NStorage {

using namespace NActors;

////////////////////////////////////////////////////////////////////////////////

void TNonreplicatedPartitionMigrationActor::HandlePartCounters(
    const TEvVolume::TEvNonreplicatedPartitionCounters::TPtr& ev,
    const TActorContext& ctx)
{
    auto* msg = ev->Get();

    if (ev->Sender == SrcActorId) {
        SrcCounters = std::move(msg->DiskCounters);
    } else if (ev->Sender == DstActorId) {
        DstCounters = std::move(msg->DiskCounters);
    } else {
        LOG_INFO(ctx, TBlockStoreComponents::PARTITION,
            "Partition %s for disk %s counters not found",
            ToString(ev->Sender).c_str(),
            SrcConfig->GetName().Quote().c_str());

        Y_VERIFY_DEBUG(0);
    }
}

////////////////////////////////////////////////////////////////////////////////

void TNonreplicatedPartitionMigrationActor::SendStats(const TActorContext& ctx)
{
    auto stats = CreatePartitionDiskCounters(EPublishingPolicy::NonRepl);

    if (SrcCounters) {
        stats->AggregateWith(*SrcCounters);
    }

    if (DstActorId && DstCounters) {
        stats->AggregateWith(*DstCounters);
    }

    if (SrcCounters && DstActorId && DstCounters) {
        // for some counters default AggregateWith logic is suboptimal for
        // mirrored partitions
        stats->Simple.BytesCount.Value = Max(
            SrcCounters->Simple.BytesCount.Value,
            DstCounters->Simple.BytesCount.Value);
        stats->Simple.IORequestsInFlight.Value = Max(
            SrcCounters->Simple.IORequestsInFlight.Value,
            DstCounters->Simple.IORequestsInFlight.Value);
    }

    auto request = std::make_unique<TEvVolume::TEvNonreplicatedPartitionCounters>(
        MakeIntrusive<TCallContext>(),
        std::move(stats));

    NCloud::Send(ctx, StatActorId, std::move(request));
}

}   // namespace NCloud::NBlockStore::NStorage
