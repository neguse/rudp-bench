cmake_minimum_required(VERSION 3.16)

project(rudp_bench_gns C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)

set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/gns gns-build EXCLUDE_FROM_ALL)

# Valve が出荷する版と同じにする。_CERT がないと Release でも内部の検査
# (STEAMNETWORKINGSOCKETS_SNP_PARANOIA など)が有効になる
# (src/steamnetworkingsockets/steamnetworkingsockets_internal.h)。
target_compile_definitions(GameNetworkingSockets_s PRIVATE _CERT)

find_package(Threads REQUIRED)

# gns_timeout30_sut は切断までの時間を 30 秒にする(gns.cc)
foreach(target IN ITEMS gns_sut gns_timeout30_sut)
  add_executable(${target} gns.cc)
  target_link_libraries(${target} PRIVATE benchkit GameNetworkingSockets_s Threads::Threads)
  target_include_directories(${target} SYSTEM PRIVATE ../../third_party/gns/include)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
target_compile_definitions(gns_timeout30_sut PRIVATE GNS_TIMEOUT30)
