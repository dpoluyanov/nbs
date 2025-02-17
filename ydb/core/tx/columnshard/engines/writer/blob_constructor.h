#pragma once

#include <library/cpp/actors/core/event.h>
#include <ydb/core/protos/base.pb.h>


namespace NKikimr {

struct TAppData;

namespace NColumnShard {

class TBlobBatch;
struct TUsage;

}

namespace NOlap {

class TUnifiedBlobId;

class IBlobConstructor {
public:
    using TPtr = std::shared_ptr<IBlobConstructor>;

    enum class EStatus {
        Ok,
        Finished,
        Error
    };

    virtual ~IBlobConstructor() {}
    virtual const TString& GetBlob() const = 0;
    virtual bool RegisterBlobId(const TUnifiedBlobId& blobId) = 0;
    virtual EStatus BuildNext() = 0;
};

}
}
