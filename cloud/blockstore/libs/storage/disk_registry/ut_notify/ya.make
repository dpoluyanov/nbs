UNITTEST_FOR(cloud/blockstore/libs/storage/disk_registry)

INCLUDE(${ARCADIA_ROOT}/cloud/blockstore/tests/recipes/small.inc)

SRCS(
    disk_registry_ut_notify.cpp
)

PEERDIR(
    cloud/blockstore/config
    cloud/blockstore/libs/storage/api
    cloud/blockstore/libs/storage/disk_registry/testlib
    cloud/blockstore/libs/storage/testlib
    library/cpp/testing/unittest
    ydb/core/testlib/basics
)

END()
