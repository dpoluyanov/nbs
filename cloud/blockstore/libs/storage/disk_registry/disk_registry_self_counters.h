#pragma once

#include "public.h"

#include <cloud/storage/core/libs/diagnostics/solomon_counters.h>

#include <library/cpp/monlib/dynamic_counters/counters.h>

namespace NCloud::NBlockStore::NStorage {

////////////////////////////////////////////////////////////////////////////////

struct TDiskRegistrySelfCounters
{
    using TCounterPtr = NMonitoring::TDynamicCounters::TCounterPtr;

    struct TDevicePoolCounters
    {
        TCounterPtr FreeBytes;
        TCounterPtr TotalBytes;
        TCounterPtr AllocatedDevices;
        TCounterPtr DirtyDevices;
        TCounterPtr DevicesInOnlineState;
        TCounterPtr DevicesInWarningState;
        TCounterPtr DevicesInErrorState;

        void Init(NMonitoring::TDynamicCountersPtr counters);
    };

    struct TNonreplMetricsCounter
    {
        TCounterPtr CountCounter;
        TCounterPtr CountByteCounter;
    };

    TCounterPtr FreeBytes;
    TCounterPtr TotalBytes;
    TCounterPtr AllocatedDisks;
    TCounterPtr AllocatedDevices;
    TCounterPtr DirtyDevices;
    TCounterPtr DevicesInOnlineState;
    TCounterPtr DevicesInWarningState;
    TCounterPtr DevicesInErrorState;
    TCounterPtr AgentsInOnlineState;
    TCounterPtr AgentsInWarningState;
    TCounterPtr AgentsInUnavailableState;
    TCounterPtr DisksInOnlineState;
    TCounterPtr DisksInMigrationState;
    TCounterPtr DevicesInMigrationState;
    TCounterPtr DisksInTemporarilyUnavailableState;
    TCounterPtr DisksInErrorState;
    TCounterPtr PlacementGroups;
    TCounterPtr FullPlacementGroups;
    TCounterPtr AllocatedDisksInGroups;
    TCounterPtr Mirror2Disks;
    TCounterPtr Mirror2DisksMinus1;
    TCounterPtr Mirror2DisksMinus2;
    TCounterPtr Mirror3Disks;
    TCounterPtr Mirror3DisksMinus1;
    TCounterPtr Mirror3DisksMinus2;
    TCounterPtr Mirror3DisksMinus3;
    TCounterPtr MaxMigrationTime;
    TCounterPtr PlacementGroupsWithRecentlyBrokenSingleDisk;
    TCounterPtr PlacementGroupsWithRecentlyBrokenTwoOrMoreDisks;
    TCounterPtr PlacementGroupsWithBrokenSingleDisk;
    TCounterPtr PlacementGroupsWithBrokenTwoOrMoreDisks;
    TCounterPtr MeanTimeBetweenFailures;
    TCounterPtr AutomaticallyReplacedDevices;

    TCumulativeCounter QueryAvailableStorageErrors;

    TDevicePoolCounters DefaultPoolCounters;
    TDevicePoolCounters LocalPoolCounters;

    TVector<TNonreplMetricsCounter> NonreplMetricsCounter;

    void Init(NMonitoring::TDynamicCountersPtr counters);
};

}   // namespace NCloud::NBlockStore::NStorage
