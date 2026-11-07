cmake_minimum_required(VERSION 3.16)

project(rudp_bench_yojimbo C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)

set(YOJIMBO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YOJIMBO_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/yojimbo yojimbo-build EXCLUDE_FROM_ALL)

add_executable(yojimbo_sut yojimbo.cc)
target_link_libraries(yojimbo_sut PRIVATE benchkit yojimbo)
target_compile_options(yojimbo_sut PRIVATE -Wall -Wextra -Wpedantic -Werror)
