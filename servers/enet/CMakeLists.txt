cmake_minimum_required(VERSION 3.16)

project(rudp_bench_enet C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)
add_subdirectory(../../third_party/enet enet-build)
target_compile_definitions(enet PRIVATE _DEFAULT_SOURCE)

add_executable(enet_sut enet.c)
target_link_libraries(enet_sut PRIVATE benchkit enet)
target_include_directories(enet_sut SYSTEM PRIVATE ../../third_party/enet/include)
target_compile_options(enet_sut PRIVATE -Wall -Wextra -Wpedantic -Werror)
