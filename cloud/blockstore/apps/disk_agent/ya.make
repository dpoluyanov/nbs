PROGRAM(diskagentd)

ALLOCATOR(TCMALLOC_TC)

SRCS(
    main.cpp
)

PEERDIR(
    cloud/blockstore/libs/disk_agent
    cloud/blockstore/libs/rdma/impl
    cloud/blockstore/libs/spdk/iface

    cloud/storage/core/libs/daemon

    ydb/core/driver_lib/run
    ydb/core/security

    library/cpp/getopt
)

IF (BUILD_TYPE != "PROFILE")
    SPLIT_DWARF()
ENDIF()

YQL_LAST_ABI_VERSION()

INCLUDE(${ARCADIA_ROOT}/cloud/blockstore/apps/common/restrict.inc)

END()
