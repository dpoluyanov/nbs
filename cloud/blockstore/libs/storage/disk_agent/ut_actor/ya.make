UNITTEST_FOR(cloud/blockstore/libs/storage/disk_agent)

INCLUDE(${ARCADIA_ROOT}/cloud/blockstore/tests/recipes/small.inc)

SRCS(
    disk_agent_actor_ut.cpp
)

PEERDIR(
    cloud/blockstore/libs/storage/api
    cloud/blockstore/libs/storage/core
    cloud/blockstore/libs/storage/disk_agent/testlib
    library/cpp/actors/core
    library/cpp/testing/gmock_in_unittest
    library/cpp/testing/unittest
    ydb/core/testlib
    ydb/core/testlib/basics
)

END()
