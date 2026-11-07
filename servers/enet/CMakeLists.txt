cmake_minimum_required(VERSION 3.16)

project(rudp_bench_enet C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)
add_subdirectory(../../third_party/enet enet-build)
target_compile_definitions(enet PRIVATE _DEFAULT_SOURCE)

# enet_timeout30_sut は切断までの時間を 30 秒にする(enet.c)
foreach(target IN ITEMS enet_sut enet_timeout30_sut)
  add_executable(${target} enet.c)
  target_link_libraries(${target} PRIVATE benchkit enet)
  target_include_directories(${target} SYSTEM PRIVATE ../../third_party/enet/include)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
target_compile_definitions(enet_timeout30_sut PRIVATE ENET_TIMEOUT30)
