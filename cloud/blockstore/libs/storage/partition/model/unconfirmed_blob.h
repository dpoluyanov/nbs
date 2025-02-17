#pragma once

#include "public.h"

#include <cloud/blockstore/libs/common/block_range.h>

#include <util/generic/hash.h>
#include <util/generic/vector.h>

namespace NCloud::NBlockStore::NStorage::NPartition {

////////////////////////////////////////////////////////////////////////////////

struct TUnconfirmedBlob
{
    ui64 UniqueId = 0;
    TBlockRange32 BlockRange;

    TUnconfirmedBlob() = default;

    TUnconfirmedBlob(ui64 uniqueId, const TBlockRange32& blockRange)
        : UniqueId(uniqueId)
        , BlockRange(blockRange)
    {}
};

// mapping from CommitId
using TUnconfirmedBlobs = THashMap<ui64, TVector<TUnconfirmedBlob>>;
using TConfirmedBlobs = THashMap<ui64, TVector<TUnconfirmedBlob>>;

}   // namespace NCloud::NBlockStore::NStorage::NPartition
