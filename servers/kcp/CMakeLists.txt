cmake_minimum_required(VERSION 3.16)

project(rudp_bench_kcp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)

add_library(kcp_vendor STATIC ../../third_party/kcp/ikcp.c)
target_include_directories(kcp_vendor PUBLIC ../../third_party/kcp)

add_executable(kcp_sut kcp.c)
target_link_libraries(kcp_sut PRIVATE benchkit kcp_vendor)
target_compile_definitions(kcp_sut PRIVATE _DEFAULT_SOURCE)
target_compile_options(kcp_sut PRIVATE -Wall -Wextra -Wpedantic -Werror)
