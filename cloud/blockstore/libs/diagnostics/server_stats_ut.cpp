#include "server_stats.h"

#include "config.h"
#include "dumpable.h"
#include "profile_log.h"
#include "request_stats.h"
#include "volume_stats.h"

#include <cloud/blockstore/libs/service/context.h>

#include <cloud/storage/core/libs/common/timer_test.h>
#include <cloud/storage/core/libs/diagnostics/logging.h>
#include <cloud/storage/core/libs/diagnostics/monitoring.h>

#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NCloud::NBlockStore {

////////////////////////////////////////////////////////////////////////////////

namespace {

struct TTestDumpable
    : public IDumpable
{
    void Dump(IOutputStream& out) const override
    {
        Y_UNUSED(out);
    };

    void DumpHtml(IOutputStream& out) const override
    {
        Y_UNUSED(out);
    }
};

////////////////////////////////////////////////////////////////////////////////


auto UpdateStatsWithRequestResultedInRetriableError(
    IServerStatsPtr serverStats,
    IMonitoringServicePtr monitoring,
    bool silenceRetriableErrors,
    bool isHwProblem)
{
    TLog log;

    TMetricRequest request {EBlockStoreRequest::WriteBlocks};
    serverStats->PrepareMetricRequest(
        request,
        "client",
        "volume",
        0,
        4096,
        false);

    auto callContext = MakeIntrusive<TCallContext>();
    callContext->SetSilenceRetriableErrors(silenceRetriableErrors);

    serverStats->RequestCompleted(
        log,
        request,
        *callContext,
        MakeError(
            E_REJECTED,
            "Volume not ready",
            isHwProblem ? NCloud::NProto::EF_HW_PROBLEMS_DETECTED : 0));

    serverStats->UpdateStats(true);

    return monitoring->GetCounters()
        ->GetSubgroup("counters", "blockstore")
        ->GetSubgroup("component", "server_volume")
        ->GetSubgroup("host", "cluster")
        ->GetSubgroup("volume", "volume")
        ->GetSubgroup("instance", "instance");
}

void CheckRetriableError(
    IServerStatsPtr serverStats,
    IMonitoringServicePtr monitoring,
    bool silenceRetriableErrors,
    ui64 expected)
{
    auto instanceCounters =
        UpdateStatsWithRequestResultedInRetriableError(
            serverStats,
            monitoring,
            silenceRetriableErrors,
            false /*not a hw problem*/);

    UNIT_ASSERT_VALUES_EQUAL(
        expected,
        instanceCounters
        ->GetSubgroup("request", "WriteBlocks")
        ->GetCounter("Errors/Retriable", true)->Val());
}

void CheckHwProblems(
    IServerStatsPtr serverStats,
    IMonitoringServicePtr monitoring,
    bool silenceRetriableErrors,
    bool isHwProblem,
    ui64 expected)
{
    auto instanceCounters =
        UpdateStatsWithRequestResultedInRetriableError(
            serverStats,
            monitoring,
            silenceRetriableErrors,
            isHwProblem);

    UNIT_ASSERT_VALUES_EQUAL(
        expected,
        instanceCounters->GetCounter("HwProblems")->Val());
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TServerStatsTest)
{
    Y_UNIT_TEST(ShouldTrackIncompleteRequestsPerVolume)
    {
        auto timer = std::make_shared<TTestTimer>();
        auto monitoring = CreateMonitoringServiceStub();

        auto serverGroup = monitoring
            ->GetCounters()
            ->GetSubgroup("counters", "blockstore");

        auto volumeStats = CreateVolumeStats(
            monitoring,
            {},
            EVolumeStatsType::EServerStats,
            CreateWallClockTimer());

        auto serverStats = CreateServerStats(
            std::make_shared<TTestDumpable>(),
            std::make_shared<TDiagnosticsConfig>(),
            monitoring,
            CreateProfileLogStub(),
            CreateServerRequestStats(serverGroup,timer),
            std::move(volumeStats)
        );

        NProto::TVolume volume;
        volume.SetDiskId("volume");
        serverStats->MountVolume(volume, "client", "instance");

        TMetricRequest request {EBlockStoreRequest::WriteBlocks};
        serverStats->PrepareMetricRequest(
            request,
            "client",
            "volume",
            0,
            4096,
            false);

        UNIT_ASSERT_VALUES_UNEQUAL(0, request.VolumeInfo.use_count());

        serverStats->AddIncompleteRequest(
            request.VolumeInfo,
            NProto::STORAGE_MEDIA_HYBRID,
            EBlockStoreRequest::WriteBlocks,
            TRequestTime{
                .TotalTime = TDuration::Hours(1),
                .ExecutionTime = TDuration::Hours(1)
            }
        );

        serverStats->UpdateStats(false);

        UNIT_ASSERT_VALUES_EQUAL(
            TDuration::Hours(1).MicroSeconds(),
            monitoring
            ->GetCounters()
            ->GetSubgroup("counters", "blockstore")
            ->GetSubgroup("component", "server_volume")
            ->GetSubgroup("host", "cluster")
            ->GetSubgroup("volume", "volume")
            ->GetSubgroup("instance", "instance")
            ->GetSubgroup("request", "WriteBlocks")
            ->GetCounter("MaxTime")->Val());
    }

    Y_UNIT_TEST(ShouldSilenceErrorsIfCallContextHasSilenceRetriable)
    {
        auto timer = std::make_shared<TTestTimer>();
        auto monitoring = CreateMonitoringServiceStub();

        auto serverGroup = monitoring
            ->GetCounters()
            ->GetSubgroup("counters", "blockstore");

        auto volumeStats = CreateVolumeStats(
            monitoring,
            {},
            EVolumeStatsType::EServerStats,
            CreateWallClockTimer());

        auto serverStats = CreateServerStats(
            std::make_shared<TTestDumpable>(),
            std::make_shared<TDiagnosticsConfig>(),
            monitoring,
            CreateProfileLogStub(),
            CreateServerRequestStats(serverGroup,timer),
            std::move(volumeStats)
        );

        NProto::TVolume volume;
        volume.SetBlockSize(4096);
        volume.SetDiskId("volume");
        serverStats->MountVolume(volume, "client", "instance");

        CheckRetriableError(serverStats, monitoring, false, 1);
        CheckRetriableError(serverStats, monitoring, true, 1);
    }

    Y_UNIT_TEST(ShouldCountHwProblems)
    {
        auto timer = std::make_shared<TTestTimer>();
        auto monitoring = CreateMonitoringServiceStub();

        auto serverGroup = monitoring
            ->GetCounters()
            ->GetSubgroup("counters", "blockstore");

        auto volumeStats = CreateVolumeStats(
            monitoring,
            {},
            EVolumeStatsType::EServerStats,
            CreateWallClockTimer());

        auto serverStats = CreateServerStats(
            std::make_shared<TTestDumpable>(),
            std::make_shared<TDiagnosticsConfig>(),
            monitoring,
            CreateProfileLogStub(),
            CreateServerRequestStats(serverGroup,timer),
            std::move(volumeStats)
        );

        NProto::TVolume volume;
        volume.SetBlockSize(4096);
        volume.SetDiskId("volume");
        volume.SetStorageMediaKind(
            NCloud::NProto::EStorageMediaKind::STORAGE_MEDIA_SSD_NONREPLICATED);
        serverStats->MountVolume(volume, "client", "instance");

        CheckHwProblems(serverStats, monitoring, false, false, 0);
        CheckHwProblems(serverStats, monitoring, true, false, 0);
        CheckHwProblems(serverStats, monitoring, false, true, 1);
        CheckHwProblems(serverStats, monitoring, true, true, 2);
    }
}

}   // namespace NCloud::NBlockStore
