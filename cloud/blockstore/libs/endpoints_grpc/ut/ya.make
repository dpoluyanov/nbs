UNITTEST_FOR(cloud/blockstore/libs/endpoints_grpc)

INCLUDE(${ARCADIA_ROOT}/cloud/blockstore/tests/recipes/small.inc)

SRCS(
    socket_endpoint_listener_ut.cpp
)

END()
