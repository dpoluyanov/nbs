LIBRARY()

SRCS(
    insert_table.cpp
    rt_insertion.cpp
    data.cpp
    path_info.cpp
)

PEERDIR(
    contrib/libs/apache/arrow
    ydb/core/protos
    ydb/core/formats/arrow
    ydb/core/tablet_flat
)

END()
