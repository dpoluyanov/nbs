#include "request_counters.h"

#include "counters_helper.h"
#include "histogram_types.h"
#include "max_calculator.h"
#include "weighted_percentile.h"

#include <cloud/storage/core/libs/common/helpers.h>
#include <cloud/storage/core/libs/common/timer.h>
#include <cloud/storage/core/libs/common/verify.h>

#include <library/cpp/monlib/dynamic_counters/counters.h>

#include <util/datetime/cputimer.h>
#include <util/generic/algorithm.h>
#include <util/generic/vector.h>
#include <util/string/builder.h>

#include <utility>

namespace NCloud {

using namespace NMonitoring;

namespace {

////////////////////////////////////////////////////////////////////////////////

template <typename TDerived>
struct THistBase
{
    struct TBucket
    {
        double Value;
        TDynamicCounters::TCounterPtr Counter;

        TBucket(double value = 0)
            : Value(value)
            , Counter(new TCounterForPtr(true))
        {}
    };

    std::array<TBucket, TDerived::BUCKETS_COUNT> Buckets;

    THistBase()
    {
        std::copy(
            TDerived::Buckets.begin(),
            TDerived::Buckets.end(),
            Buckets.begin());
    }

    void Register(
        TDynamicCounters& counters,
        TCountableBase::EVisibility vis = TCountableBase::EVisibility::Public)
    {
        const auto names = TDerived::MakeNames();
        for (size_t i = 0; i < Buckets.size(); ++i) {
            Buckets[i].Counter = counters.GetCounter(names[i], true, vis);
        }
    }

    void Increment(double value, ui64 count = 1)
    {
        auto comparer = [] (const TBucket& bucket, double value) {
            return bucket.Value < value;
        };

        auto it = LowerBound(
            Buckets.begin(),
            Buckets.end(),
            value,
            comparer);
        STORAGE_VERIFY(
            it != Buckets.end(),
            "Bucket",
            value);

        it->Counter->Add(count);
    }

    TVector<TBucketInfo> GetBuckets() const
    {
        TVector<TBucketInfo> result(Reserve(Buckets.size()));
        for (const auto& bucket: Buckets) {
            result.emplace_back(
                bucket.Value,
                bucket.Counter->Val());
        }

        return result;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TTimeHist
    : public THistBase<TRequestMsTimeBuckets>
{
    void Increment(TDuration requestTime, ui64 count = 1)
    {
        THistBase::Increment(requestTime.MicroSeconds() / 1000., count);
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TSizeHist
    : public THistBase<TKbSizeBuckets>
{
    void Increment(double requestBytes, ui64 count = 1)
    {
        THistBase::Increment(requestBytes / 1024, count);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TRequestPercentiles
{
    using TDynamicCounterPtr = TDynamicCounters::TCounterPtr;

private:
    TVector<TDynamicCounterPtr> Counters;

    TVector<ui64> Prev;

public:
    void Register(TDynamicCounters& counters)
    {
        const auto& percentiles = GetDefaultPercentiles();
        for (ui32 i = 0; i < percentiles.size(); ++i) {
            Counters.emplace_back(
                counters.GetCounter(percentiles[i].second, false));
        }
    }

    void Update(const TVector<TBucketInfo>& update)
    {
        if (Prev.size() < update.size()) {
            Prev.resize(update.size());
        }

        TVector<TBucketInfo> delta(Reserve(update.size()));
        for (ui32 i = 0; i < update.size(); ++i) {
            delta.emplace_back(
                update[i].first,
                update[i].second - Prev[i]);
            Prev[i] = update[i].second;
        }

        auto result = CalculateWeightedPercentiles(
            delta,
            GetDefaultPercentiles());

        for (ui32 i = 0; i < Min(Counters.size(), result.size()); ++i) {
            *Counters[i] = std::lround(result[i]);
        }
    }
};

}   // namespace

////////////////////////////////////////////////////////////////////////////////

struct TRequestCounters::TSpecialCounters
{
    TDynamicCounters::TCounterPtr HwProblems;

    TSpecialCounters() = default;

    TSpecialCounters(const TSpecialCounters&) = delete;
    TSpecialCounters(TSpecialCounters&&) = default;

    TSpecialCounters& operator = (const TSpecialCounters&) = delete;
    TSpecialCounters& operator = (TSpecialCounters&&) = default;

    void Init(TDynamicCounters& counters)
    {
        HwProblems = counters.GetCounter("HwProblems", true);
    }

    void AddStats(
        EDiagnosticsErrorKind errorKind,
        ui32 errorFlags)
    {
        if ((errorKind != EDiagnosticsErrorKind::Success)
            && HasProtoFlag(
                errorFlags,
                NCloud::NProto::EF_HW_PROBLEMS_DETECTED))
        {
            HwProblems->Inc();
        }
    }

    void AddRetryStats(
        EDiagnosticsErrorKind errorKind,
        ui32 errorFlags)
    {
        AddStats(errorKind, errorFlags);
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TRequestCounters::TStatCounters
{
    template<typename Type>
    struct TFailedAndSuccess
    {
        Type Success;
        Type Failed;
    };

    bool IsReadWriteRequest = false;
    bool ReportDataPlaneHistogram = false;
    bool ReportControlPlaneHistogram = false;

    TDynamicCounters::TCounterPtr Count;
    TDynamicCounters::TCounterPtr MaxCount;
    TDynamicCounters::TCounterPtr UnalignedCount;
    TDynamicCounters::TCounterPtr Time;
    TDynamicCounters::TCounterPtr MaxTime;
    TDynamicCounters::TCounterPtr MaxTotalTime;
    TDynamicCounters::TCounterPtr MaxSize;
    TDynamicCounters::TCounterPtr RequestBytes;
    TDynamicCounters::TCounterPtr MaxRequestBytes;
    TDynamicCounters::TCounterPtr InProgress;
    TDynamicCounters::TCounterPtr MaxInProgress;
    TDynamicCounters::TCounterPtr InProgressBytes;
    TDynamicCounters::TCounterPtr MaxInProgressBytes;
    TDynamicCounters::TCounterPtr PostponedQueueSize;
    TDynamicCounters::TCounterPtr MaxPostponedQueueSize;
    TDynamicCounters::TCounterPtr PostponedCount;
    TDynamicCounters::TCounterPtr PostponedQueueSizeGrpc;
    TDynamicCounters::TCounterPtr MaxPostponedQueueSizeGrpc;
    TDynamicCounters::TCounterPtr PostponedCountGrpc;
    TDynamicCounters::TCounterPtr FastPathHits;

    TDynamicCounters::TCounterPtr Errors;
    TDynamicCounters::TCounterPtr ErrorsAborted;
    TDynamicCounters::TCounterPtr ErrorsFatal;
    TDynamicCounters::TCounterPtr ErrorsRetriable;
    TDynamicCounters::TCounterPtr ErrorsThrottling;
    TDynamicCounters::TCounterPtr ErrorsSession;
    TDynamicCounters::TCounterPtr ErrorsSilent;

    TDynamicCounters::TCounterPtr Retries;

    TSizeHist SizeHist;
    TRequestPercentiles SizePercentiles;

    TTimeHist TimeHist;
    TTimeHist TimeHistUnaligned;
    TRequestPercentiles TimePercentiles;

    TTimeHist ExecutionTimeHist;
    TTimeHist ExecutionTimeHistUnaligned;
    TRequestPercentiles ExecutionTimePercentiles;

    TTimeHist PostponedTimeHist;
    TRequestPercentiles PostponedTimePercentiles;

    TMaxCalculator<DEFAULT_BUCKET_COUNT> MaxTimeCalc;
    TMaxCalculator<DEFAULT_BUCKET_COUNT> MaxTotalTimeCalc;
    TMaxCalculator<DEFAULT_BUCKET_COUNT> MaxSizeCalc;
    TMaxCalculator<DEFAULT_BUCKET_COUNT> MaxInProgressCalc;
    TMaxCalculator<DEFAULT_BUCKET_COUNT> MaxInProgressBytesCalc;
    TMaxCalculator<DEFAULT_BUCKET_COUNT> MaxPostponedQueueSizeCalc;
    TMaxCalculator<DEFAULT_BUCKET_COUNT> MaxPostponedQueueSizeGrpcCalc;
    TMaxPerSecondCalculator<DEFAULT_BUCKET_COUNT> MaxCountCalc;
    TMaxPerSecondCalculator<DEFAULT_BUCKET_COUNT> MaxRequestBytesCalc;

    explicit TStatCounters(ITimerPtr timer)
        : MaxTimeCalc(timer)
        , MaxTotalTimeCalc(timer)
        , MaxSizeCalc(timer)
        , MaxInProgressCalc(timer)
        , MaxInProgressBytesCalc(timer)
        , MaxPostponedQueueSizeCalc(timer)
        , MaxPostponedQueueSizeGrpcCalc(timer)
        , MaxCountCalc(timer)
        , MaxRequestBytesCalc(timer)
    {}

    TStatCounters(const TStatCounters&) = delete;
    TStatCounters(TStatCounters&&) = default;

    TStatCounters& operator = (const TStatCounters&) = delete;
    TStatCounters& operator = (TStatCounters&&) = default;

    void Init(
        TDynamicCounters& counters,
        bool isReadWriteRequest,
        bool reportDataPlaneHistogram,
        bool reportControlPlaneHistogram)
    {
        IsReadWriteRequest = isReadWriteRequest;
        ReportDataPlaneHistogram = reportDataPlaneHistogram;
        ReportControlPlaneHistogram = reportControlPlaneHistogram;

        Count = counters.GetCounter("Count", true);

        Time = counters.GetCounter("Time", true);
        MaxTime = counters.GetCounter("MaxTime");
        MaxTotalTime = counters.GetCounter("MaxTotalTime");

        InProgress = counters.GetCounter("InProgress");
        MaxInProgress = counters.GetCounter("MaxInProgress");

        Errors = counters.GetCounter("Errors", true);
        ErrorsAborted = counters.GetCounter("Errors/Aborted", true);
        ErrorsFatal = counters.GetCounter("Errors/Fatal", true);
        ErrorsRetriable = counters.GetCounter("Errors/Retriable", true);
        ErrorsThrottling = counters.GetCounter("Errors/Throttling", true);
        ErrorsSession = counters.GetCounter("Errors/Session", true);
        Retries = counters.GetCounter("Retries", true);

        if (IsReadWriteRequest) {
            ErrorsSilent = counters.GetCounter("Errors/Silent", true);

            MaxSize = counters.GetCounter("MaxSize");
            MaxCount = counters.GetCounter("MaxCount");

            RequestBytes = counters.GetCounter("RequestBytes", true);
            MaxRequestBytes = counters.GetCounter("MaxRequestBytes");

            InProgressBytes = counters.GetCounter("InProgressBytes");
            MaxInProgressBytes = counters.GetCounter("MaxInProgressBytes");

            UnalignedCount = counters.GetCounter("UnalignedCount", true);

            if (ReportDataPlaneHistogram) {
                auto unalignedClassGroup = counters.GetSubgroup("sizeclass", "Unaligned");

                SizeHist.Register(*counters.GetSubgroup("histogram", "Size"));
                TimeHistUnaligned.Register(*unalignedClassGroup->GetSubgroup("histogram", "Time"));
                ExecutionTimeHist.Register(
                    *counters.GetSubgroup("histogram", "ExecutionTime"));
                ExecutionTimeHistUnaligned.Register(
                    *unalignedClassGroup->GetSubgroup("histogram", "ExecutionTime"));
            } else {
                SizePercentiles.Register(*counters.GetSubgroup("percentiles", "Size"));
                ExecutionTimePercentiles.Register(
                    *counters.GetSubgroup("percentiles", "ExecutionTime"));
                PostponedTimePercentiles.Register(
                    *counters.GetSubgroup("percentiles", "ThrottlerDelay"));
                TimePercentiles.Register(*counters.GetSubgroup("percentiles", "Time"));
            }

            const auto visibleHistogram = ReportDataPlaneHistogram
                ? TCountableBase::EVisibility::Public
                : TCountableBase::EVisibility::Private;

            PostponedTimeHist.Register(*MakeVisibilitySubgroup(
                counters,
                "histogram",
                "ThrottlerDelay",
                visibleHistogram), visibleHistogram);

            TimeHist.Register(*MakeVisibilitySubgroup(
                counters,
                "histogram",
                "Time",
                visibleHistogram), visibleHistogram);

            PostponedQueueSize = counters.GetCounter("PostponedQueueSize");
            MaxPostponedQueueSize = counters.GetCounter("MaxPostponedQueueSize");
            PostponedCount = counters.GetCounter("PostponedCount", true);

            PostponedQueueSizeGrpc =
                counters.GetCounter("PostponedQueueSizeGrpc");
            MaxPostponedQueueSizeGrpc =
                counters.GetCounter("MaxPostponedQueueSizeGrpc");
            PostponedCountGrpc =
                counters.GetCounter("PostponedCountGrpc", true);

            FastPathHits = counters.GetCounter("FastPathHits", true);
        } else {
            if (ReportControlPlaneHistogram) {
                TimeHist.Register(*counters.GetSubgroup(
                    "histogram",
                    "Time"));
            } else {
                TimePercentiles.Register(
                    *counters.GetSubgroup("percentiles", "Time"));
            }
        }
    }

    void Started(ui32 requestBytes)
    {
        MaxInProgressCalc.Add(InProgress->Inc());

        if (IsReadWriteRequest) {
            MaxInProgressBytesCalc.Add(InProgressBytes->Add(requestBytes));
        }
    }

    void Completed(ui32 requestBytes)
    {
        InProgress->Dec();

        if (IsReadWriteRequest) {
            InProgressBytes->Sub(requestBytes);
        }
    }

    void AddStats(
        TDuration requestTime,
        TDuration postponedTime,
        ui32 requestBytes,
        EDiagnosticsErrorKind errorKind,
        bool unaligned,
        ECalcMaxTime calcMaxTime)
    {
        const bool failed = errorKind != EDiagnosticsErrorKind::Success
            && (errorKind != EDiagnosticsErrorKind::ErrorSilent
                || !IsReadWriteRequest);

        if (failed) {
            Errors->Inc();
        } else {
            Count->Inc();
        }

        switch (errorKind) {
            case EDiagnosticsErrorKind::Success:
                break;
            case EDiagnosticsErrorKind::ErrorAborted:
                ErrorsAborted->Inc();
                break;
            case EDiagnosticsErrorKind::ErrorFatal:
                ErrorsFatal->Inc();
                break;
            case EDiagnosticsErrorKind::ErrorRetriable:
                ErrorsRetriable->Inc();
                break;
            case EDiagnosticsErrorKind::ErrorThrottling:
                ErrorsThrottling->Inc();
                break;
            case EDiagnosticsErrorKind::ErrorSession:
                ErrorsSession->Inc();
                break;
            case EDiagnosticsErrorKind::ErrorSilent:
                if (IsReadWriteRequest) {
                    ErrorsSilent->Inc();
                }
                break;
        }

        auto execTime = requestTime - postponedTime;
        Time->Add(requestTime.MicroSeconds());
        TimeHist.Increment(requestTime);
        if (calcMaxTime == ECalcMaxTime::ENABLE) {
            MaxTimeCalc.Add(execTime.MicroSeconds());
        }
        MaxTotalTimeCalc.Add(requestTime.MicroSeconds());

        if (IsReadWriteRequest) {
            MaxCountCalc.Add(1);
            RequestBytes->Add(requestBytes);
            MaxRequestBytesCalc.Add(requestBytes);

            SizeHist.Increment(requestBytes);
            MaxSizeCalc.Add(requestBytes);

            if (unaligned) {
                UnalignedCount->Inc();
                TimeHistUnaligned.Increment(requestTime);
                ExecutionTimeHistUnaligned.Increment(execTime);
            }

            ExecutionTimeHist.Increment(execTime);
            PostponedTimeHist.Increment(postponedTime);
        }
    }

    void BatchCompleted(
        ui64 requestCount,
        ui64 bytes,
        ui64 errors,
        std::span<TTimeBucket> timeHist,
        std::span<TSizeBucket> sizeHist,
        bool unaligned)
    {
        Count->Add(requestCount);
        Errors->Add(errors);

        for (auto [dt, count]: timeHist) {
            Time->Add(dt.MicroSeconds());
            TimeHist.Increment(dt, count);
            MaxTimeCalc.Add(dt.MicroSeconds());
        }

        if (IsReadWriteRequest) {
            MaxCountCalc.Add(requestCount);
            RequestBytes->Add(bytes);
            MaxRequestBytesCalc.Add(bytes);

            for (auto [size, count]: sizeHist) {
                SizeHist.Increment(size, count);
                MaxSizeCalc.Add(size);
            }

            if (unaligned) {
                UnalignedCount->Add(requestCount);
                UnalignedCount->Add(errors);
            }

            for (auto [dt, count]: timeHist) {
                ExecutionTimeHist.Increment(dt, count);

                if (unaligned) {
                    TimeHistUnaligned.Increment(dt, count);
                    ExecutionTimeHistUnaligned.Increment(dt, count);
                }
            }
        }
    }

    void AddRetryStats(EDiagnosticsErrorKind errorKind)
    {
        switch (errorKind) {
            case EDiagnosticsErrorKind::ErrorRetriable:
                ErrorsRetriable->Inc();
                break;
            case EDiagnosticsErrorKind::ErrorThrottling:
                ErrorsThrottling->Inc();
                break;
            case EDiagnosticsErrorKind::ErrorSession:
                ErrorsSession->Inc();
                break;
            case EDiagnosticsErrorKind::Success:
            case EDiagnosticsErrorKind::ErrorAborted:
            case EDiagnosticsErrorKind::ErrorFatal:
            case EDiagnosticsErrorKind::ErrorSilent:
                Y_VERIFY_DEBUG(false);
                return;
        }

        Errors->Inc();
        Retries->Inc();
    }

    void RequestPostponed()
    {
        if (IsReadWriteRequest) {
            PostponedCount->Inc();
            MaxPostponedQueueSizeCalc.Add(PostponedQueueSize->Inc());
        }
    }

    void RequestPostponedServer()
    {
        if (IsReadWriteRequest) {
            PostponedCountGrpc->Inc();
            MaxPostponedQueueSizeGrpcCalc.Add(PostponedQueueSizeGrpc->Inc());
        }
    }

    void RequestFastPathHit()
    {
        if (IsReadWriteRequest) {
            FastPathHits->Inc();
        }
    }

    void RequestAdvanced()
    {
        if (IsReadWriteRequest) {
            PostponedQueueSize->Dec();
        }
    }

    void RequestAdvancedServer()
    {
        if (IsReadWriteRequest) {
            PostponedQueueSizeGrpc->Dec();
        }
    }

    void AddIncompleteStats(
        TDuration executionTime,
        TDuration totalTime,
        ECalcMaxTime calcMaxTime = ECalcMaxTime::ENABLE)
    {
        if (calcMaxTime == ECalcMaxTime::ENABLE) {
            MaxTimeCalc.Add(executionTime.MicroSeconds());
        }
        MaxTotalTimeCalc.Add(totalTime.MicroSeconds());
    }

    void UpdateStats(bool updatePercentiles)
    {
        *MaxInProgress = MaxInProgressCalc.NextValue();
        *MaxTime = MaxTimeCalc.NextValue();
        *MaxTotalTime = MaxTotalTimeCalc.NextValue();

        if (IsReadWriteRequest) {
            *MaxCount = MaxCountCalc.NextValue();
            *MaxSize = MaxSizeCalc.NextValue();
            *MaxRequestBytes = MaxRequestBytesCalc.NextValue();
            *MaxInProgressBytes = MaxInProgressBytesCalc.NextValue();
            *MaxPostponedQueueSize = MaxPostponedQueueSizeCalc.NextValue();
            *MaxPostponedQueueSizeGrpc = MaxPostponedQueueSizeGrpcCalc.NextValue();
            if (updatePercentiles && !ReportDataPlaneHistogram) {
                SizePercentiles.Update(SizeHist.GetBuckets());
                TimePercentiles.Update(TimeHist.GetBuckets());
                ExecutionTimePercentiles.Update(ExecutionTimeHist.GetBuckets());
                PostponedTimePercentiles.Update(PostponedTimeHist.GetBuckets());
            }
        } else if (updatePercentiles && !ReportControlPlaneHistogram) {
            TimePercentiles.Update(TimeHist.GetBuckets());
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TRequestCounters::TRequestCounters(
        ITimerPtr timer,
        ui32 requestCount,
        std::function<TString(TRequestType)> requestType2Name,
        std::function<bool(TRequestType)> isReadWriteRequestType,
        EOptions options)
    : RequestType2Name(std::move(requestType2Name))
    , IsReadWriteRequestType(std::move(isReadWriteRequestType))
    , Options(options)
{
    if (Options & EOption::AddSpecialCounters) {
        SpecialCounters = MakeHolder<TSpecialCounters>();
    }

    CountersByRequest.reserve(requestCount);
    for (ui32 i = 0; i < requestCount; ++i) {
        CountersByRequest.emplace_back(timer);
    }
}

TRequestCounters::~TRequestCounters()
{}

void TRequestCounters::Register(TDynamicCounters& counters)
{
    if (SpecialCounters) {
        SpecialCounters->Init(counters);
    }

    for (TRequestType t = 0; t < CountersByRequest.size(); ++t) {
        if (ShouldReport(t)) {
            auto requestGroup = counters.GetSubgroup(
                "request",
                RequestType2Name(t));

            CountersByRequest[t].Init(
                *requestGroup,
                IsReadWriteRequestType(t),
                Options & EOption::ReportDataPlaneHistogram,
                Options & EOption::ReportControlPlaneHistogram);
        }
    }
}

void TRequestCounters::Subscribe(TRequestCountersPtr subscriber)
{
    Subscribers.push_back(subscriber);
}

ui64 TRequestCounters::RequestStarted(
    TRequestType requestType,
    ui32 requestBytes)
{
    RequestStartedImpl(requestType, requestBytes);
    return GetCycleCount();
}

TDuration TRequestCounters::RequestCompleted(
    TRequestType requestType,
    ui64 requestStarted,
    TDuration postponedTime,
    ui32 requestBytes,
    EDiagnosticsErrorKind errorKind,
    ui32 errorFlags,
    bool unaligned,
    ECalcMaxTime calcMaxTime)
{
    auto requestTime = CyclesToDurationSafe(GetCycleCount() - requestStarted);
    RequestCompletedImpl(
        requestType,
        requestTime,
        postponedTime,
        requestBytes,
        errorKind,
        errorFlags,
        unaligned,
        calcMaxTime);
    return requestTime;
}

void TRequestCounters::AddRetryStats(
    TRequestType requestType,
    EDiagnosticsErrorKind errorKind,
    ui32 errorFlags)
{
    if (SpecialCounters) {
        SpecialCounters->AddRetryStats(errorKind, errorFlags);
    }

    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].AddRetryStats(errorKind);
    }
    NotifySubscribers(
        &TRequestCounters::AddRetryStats,
        requestType,
        errorKind,
        errorFlags);
}

void TRequestCounters::RequestPostponed(TRequestType requestType)
{
    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].RequestPostponed();
    }
    NotifySubscribers(
        &TRequestCounters::RequestPostponed,
        requestType);
}

void TRequestCounters::RequestPostponedServer(TRequestType requestType)
{
    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].RequestPostponedServer();
    }
    NotifySubscribers(
        &TRequestCounters::RequestPostponedServer,
        requestType);
}

void TRequestCounters::RequestFastPathHit(TRequestType requestType)
{
    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].RequestFastPathHit();
    }
    NotifySubscribers(
        &TRequestCounters::RequestFastPathHit,
        requestType);
}

void TRequestCounters::RequestAdvanced(TRequestType requestType)
{
    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].RequestAdvanced();
    }
    NotifySubscribers(
        &TRequestCounters::RequestAdvanced,
        requestType);
}

void TRequestCounters::RequestAdvancedServer(TRequestType requestType)
{
    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].RequestAdvancedServer();
    }
    NotifySubscribers(
        &TRequestCounters::RequestAdvancedServer,
        requestType);
}

void TRequestCounters::AddIncompleteStats(
    TRequestType requestType,
    TDuration executionTime,
    TDuration totalTime,
    ECalcMaxTime calcMaxTime)
{
    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].AddIncompleteStats(
            executionTime,
            totalTime,
            calcMaxTime);
    }
    NotifySubscribers(
        &TRequestCounters::AddIncompleteStats,
        requestType,
        executionTime,
        totalTime,
        calcMaxTime);
}

void TRequestCounters::BatchCompleted(
    TRequestType requestType,
    ui64 count,
    ui64 bytes,
    ui64 errors,
    std::span<TTimeBucket> timeHist,
    std::span<TSizeBucket> sizeHist)
{
    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].BatchCompleted(
            count,
            bytes,
            errors,
            timeHist,
            sizeHist,
            false); // unaligned
    }
    NotifySubscribers(
        &TRequestCounters::BatchCompleted,
        requestType,
        count,
        bytes,
        errors,
        timeHist,
        sizeHist);
}

void TRequestCounters::UpdateStats(bool updatePercentiles)
{
    for (size_t t = 0; t < CountersByRequest.size(); ++t) {
        if (ShouldReport(t)) {
            CountersByRequest[t].UpdateStats(updatePercentiles);
        }
    }
    // NOTE subscribers are updated by their owners
}

void TRequestCounters::RequestStartedImpl(
    TRequestType requestType,
    ui32 requestBytes)
{
    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].Started(requestBytes);
    }
    NotifySubscribers(
        &TRequestCounters::RequestStartedImpl,
        requestType,
        requestBytes);
}

void TRequestCounters::RequestCompletedImpl(
    TRequestType requestType,
    TDuration requestTime,
    TDuration postponedTime,
    ui32 requestBytes,
    EDiagnosticsErrorKind errorKind,
    ui32 errorFlags,
    bool unaligned,
    ECalcMaxTime calcMaxTime)
{
    if (SpecialCounters) {
        SpecialCounters->AddStats(errorKind, errorFlags);
    }

    if (ShouldReport(requestType)) {
        CountersByRequest[requestType].Completed(requestBytes);
        CountersByRequest[requestType].AddStats(
            requestTime,
            postponedTime,
            requestBytes,
            errorKind,
            unaligned,
            calcMaxTime);
    }
    NotifySubscribers(
        &TRequestCounters::RequestCompletedImpl,
        requestType,
        requestTime,
        postponedTime,
        requestBytes,
        errorKind,
        errorFlags,
        unaligned,
        calcMaxTime);
}

bool TRequestCounters::ShouldReport(TRequestType requestType) const
{
    return requestType < CountersByRequest.size() && (IsReadWriteRequestType(requestType)
        || !(Options & EOption::OnlyReadWriteRequests));
}

template<typename TMethod, typename... TArgs>
void TRequestCounters::NotifySubscribers(TMethod&& m, TArgs&&... args)
{
    for (auto& s: Subscribers) {
        (s.get()->*m)(std::forward<TArgs>(args)...);
    }
}

}   // namespace NCloud
