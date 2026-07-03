cmake_minimum_required(VERSION 3.16)

project(rudp_bench_v2_gns C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_subdirectory(../../benchkit benchkit-build)

# v1 と同じ GNS 構成: static lib のみ、examples/tests/tools なし、ICE なし
# (P2P 用で IP 計測には不要)。crypto は upstream 既定の system OpenSSL。
set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/gns gns-build EXCLUDE_FROM_ALL)

# Valve の retail/cert 相当でビルドする(--describe の tuning で開示)。
# _CERT なしだと upstream は Release でも DBGFLAG_ASSERT と
# STEAMNETWORKINGSOCKETS_SNP_PARANOIA=1 を有効化し、デバッグ計測が
# ベンチ対象に混入する(src/public/tier0/dbg.h,
# src/steamnetworkingsockets/steamnetworkingsockets_internal.h)。
target_compile_definitions(GameNetworkingSockets_s PRIVATE _CERT)

find_package(Threads REQUIRED)

set(GNS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/gns/include")

add_executable(gns_server server.cc)
target_link_libraries(gns_server PRIVATE benchkit GameNetworkingSockets_s
                      Threads::Threads)
target_include_directories(gns_server SYSTEM PRIVATE "${GNS_INCLUDE_DIR}")
target_compile_definitions(gns_server PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(gns_server PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(gns_client client.cc)
target_link_libraries(gns_client PRIVATE benchkit GameNetworkingSockets_s
                      Threads::Threads)
target_include_directories(gns_client SYSTEM PRIVATE "${GNS_INCLUDE_DIR}")
target_compile_definitions(gns_client PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(gns_client PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME gns_loopback_smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.py"
            "$<TARGET_FILE:gns_server>" "$<TARGET_FILE:gns_client>"
  )
  set_tests_properties(gns_loopback_smoke PROPERTIES TIMEOUT 60)
endif()
