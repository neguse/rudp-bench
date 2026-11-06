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

add_executable(msquic_sut msquic.cc)
target_link_libraries(msquic_sut PRIVATE benchkit msquic ${NUMA_LIB} Threads::Threads)
target_include_directories(msquic_sut SYSTEM PRIVATE ../../third_party/msquic/src/inc)
target_compile_options(msquic_sut PRIVATE -Wall -Wextra -Wpedantic -Werror)
