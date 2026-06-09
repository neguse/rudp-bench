# GNS upstream: static lib only, no shared lib, no examples/tests/tools, no ICE.
# ICE は P2P 接続用。loopback IP 計測には不要なので無効化してビルドを軽くする。
set(BUILD_STATIC_LIB  ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB  OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS       OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE        OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/gns gns_build EXCLUDE_FROM_ALL)

# Valve の retail/cert 相当でビルドする。_CERT が無いと upstream は Release でも
# DBGFLAG_ASSERT を定義し、STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL=1 (全ロックの
# 取得/解放で Plat_USTime 記録 — media_relay@50 クライアント CPU の ~64% が
# clock_gettime だった) と O(n) デバッグ assert (SSNPSenderState::DebugCheckReliable
# 等) が計測対象に混入する。ベンチは出荷相当の構成で測る。
target_compile_definitions(GameNetworkingSockets_s PRIVATE _CERT)

add_library(adapter_gns STATIC gns_adapter.cc)
target_link_libraries(adapter_gns PUBLIC rudp_bench_harness GameNetworkingSockets_s)
target_include_directories(adapter_gns PUBLIC ${CMAKE_SOURCE_DIR}/third_party/gns/include)
