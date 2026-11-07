cmake_minimum_required(VERSION 3.16)

project(rudp_bench_coop_rudp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)
add_subdirectory(../../coop_rudp coop_rudp-build)
# rudp.c は "coop_rudp/rudp.h" で include するので、リポジトリの根を渡す
target_include_directories(coop_rudp PRIVATE ../..)

add_executable(coop_rudp_sut coop_rudp.c)
target_link_libraries(coop_rudp_sut PRIVATE benchkit coop_rudp)
target_include_directories(coop_rudp_sut PRIVATE ../..)
target_compile_definitions(coop_rudp_sut PRIVATE _DEFAULT_SOURCE)
target_compile_options(coop_rudp_sut PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_executable(coop_rudp_test ../../coop_rudp/tests/rudp_test.c)
target_link_libraries(coop_rudp_test PRIVATE coop_rudp m)
target_include_directories(coop_rudp_test PRIVATE ../..)
target_compile_options(coop_rudp_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME coop_rudp_test COMMAND coop_rudp_test)
