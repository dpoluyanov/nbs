#pragma once

#include <util/generic/hash.h>

namespace NCloud::NBlockStore::NStorage {

///////////////////////////////////////////////////////////////////////////////

enum class EAllowedRequests
{
    ReadOnly,
    WriteOnly,
    ReadWrite,
};

///////////////////////////////////////////////////////////////////////////////

class TEmptyType
{
};

///////////////////////////////////////////////////////////////////////////////

class IRequestsInProgress
{
public:
    virtual bool WriteRequestInProgress() const = 0;
};

///////////////////////////////////////////////////////////////////////////////

template <typename TKey, typename TValue = TEmptyType>
class TRequestsInProgress: public IRequestsInProgress
{
public:
    struct TRequest
    {
        TValue Value;
        bool Write = false;
    };
    using TRequests = THashMap<TKey, TRequest>;

private:
    EAllowedRequests AllowedRequests;
    TRequests RequestsInProgress;
    size_t WriteRequestCount = 0;
    TKey RequestIdentityKeyCounter = {};

public:
    TRequestsInProgress(EAllowedRequests allowedRequests)
        : AllowedRequests(allowedRequests)
    {}

    ~TRequestsInProgress() = default;

    TKey GenerateRequestId()
    {
        return RequestIdentityKeyCounter++;
    }

    void SetRequestIdentityKey(TKey value)
    {
        RequestIdentityKeyCounter = value;
    }

    void AddReadRequest(const TKey& key, TValue value = {})
    {
        Y_VERIFY_DEBUG(!RequestsInProgress.contains(key));
        Y_VERIFY_DEBUG(
            AllowedRequests == EAllowedRequests::ReadOnly ||
            AllowedRequests == EAllowedRequests::ReadWrite);
        RequestsInProgress.emplace(key, TRequest{std::move(value), false});
    }

    TKey AddReadRequest(TValue value)
    {
        TKey key = RequestIdentityKeyCounter++;
        AddReadRequest(key, std::move(value));
        return key;
    }

    void AddWriteRequest(const TKey& key, TValue value = {})
    {
        Y_VERIFY_DEBUG(!RequestsInProgress.contains(key));
        Y_VERIFY_DEBUG(
            AllowedRequests == EAllowedRequests::WriteOnly ||
            AllowedRequests == EAllowedRequests::ReadWrite);
        RequestsInProgress.emplace(key, TRequest{std::move(value), true});
        ++WriteRequestCount;
    }

    TKey AddWriteRequest(TValue value)
    {
        TKey key = RequestIdentityKeyCounter++;
        AddWriteRequest(key, std::move(value));
        return key;
    }

    TValue GetRequest(const TKey& key) const
    {
        if (auto* requestInfo = RequestsInProgress.FindPtr(key)) {
            return requestInfo->Value;
        } else {
            Y_VERIFY_DEBUG(0);
        }
        return {};
    }

    bool RemoveRequest(const TKey& key)
    {
        auto it = RequestsInProgress.find(key);

        if (it == RequestsInProgress.end()) {
            Y_VERIFY_DEBUG(0);
            return false;
        }

        if (it->second.Write) {
            --WriteRequestCount;
        }
        RequestsInProgress.erase(it);
        return true;
    }

    const TRequests& AllRequests() const
    {
        return RequestsInProgress;
    }

    size_t GetRequestCount() const
    {
        return RequestsInProgress.size();
    }

    bool Empty() const
    {
        return GetRequestCount() == 0;
    }

    bool WriteRequestInProgress() const override
    {
        return WriteRequestCount != 0;
    }
};

}  // namespace NCloud::NBlockStore::NStorage
