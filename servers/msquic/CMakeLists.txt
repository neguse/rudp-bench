cmake_minimum_required(VERSION 3.16)

project(rudp_bench_msquic C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)

set(QUIC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_PERF OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(QUIC_TLS_LIB "quictls" CACHE STRING "" FORCE)
set(QUIC_ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/msquic msquic-build EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)
find_library(NUMA_LIB numa REQUIRED)

# libmsquic.a が libnuma を使うので、msquic の後ろに並ぶよう msquic 側に付ける
target_link_libraries(msquic INTERFACE ${NUMA_LIB} Threads::Threads)

# msquic_cancel_on_blocked_sut は LT の datagram を CANCEL_ON_BLOCKED で送る(msquic.cc)
foreach(target IN ITEMS msquic_sut msquic_cancel_on_blocked_sut)
  add_executable(${target} msquic.cc)
  target_link_libraries(${target} PRIVATE benchkit msquic)
  target_include_directories(${target} SYSTEM PRIVATE ../../third_party/msquic/src/inc)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
target_compile_definitions(msquic_cancel_on_blocked_sut PRIVATE MSQUIC_CANCEL_ON_BLOCKED)
