# GNS upstream: static lib only, no shared lib, no examples/tests/tools, no ICE.
# ICE は P2P 接続用。loopback IP 計測には不要なので無効化してビルドを軽くする。
set(BUILD_STATIC_LIB  ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB  OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS       OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE        OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/gns gns_build EXCLUDE_FROM_ALL)

add_library(adapter_gns STATIC gns_adapter.cc)
target_link_libraries(adapter_gns PUBLIC rudp_bench_harness GameNetworkingSockets_s)
target_include_directories(adapter_gns PUBLIC ${CMAKE_SOURCE_DIR}/third_party/gns/include)
