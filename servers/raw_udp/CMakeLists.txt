cmake_minimum_required(VERSION 3.16)

project(rudp_bench_raw_udp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)

add_executable(raw_udp_sut raw_udp.c)
target_link_libraries(raw_udp_sut PRIVATE benchkit)
target_compile_definitions(raw_udp_sut PRIVATE _DEFAULT_SOURCE)
target_compile_options(raw_udp_sut PRIVATE -Wall -Wextra -Wpedantic -Werror)
