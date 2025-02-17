PROGRAM()

GENERATE_ENUM_SERIALIZATION(options.h)

SRCS(
    main.cpp
    options.cpp
)

PEERDIR(
    cloud/storage/core/libs/common

    library/cpp/digest/crc32c
    library/cpp/getopt
)

INCLUDE(${ARCADIA_ROOT}/cloud/blockstore/apps/common/restrict.inc)

END()
