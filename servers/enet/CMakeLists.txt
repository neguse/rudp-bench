cmake_minimum_required(VERSION 3.16)

project(rudp_bench_enet C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)
add_subdirectory(../../third_party/enet enet-build)
target_compile_definitions(enet PRIVATE _DEFAULT_SOURCE)

# enet_legacy_sut は 2026-11-05 の測定と同じ設定(enet.c の ENET_LEGACY)
foreach(target IN ITEMS enet_sut enet_legacy_sut)
  add_executable(${target} enet.c)
  target_link_libraries(${target} PRIVATE benchkit enet)
  target_include_directories(${target} SYSTEM PRIVATE ../../third_party/enet/include)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
target_compile_definitions(enet_legacy_sut PRIVATE ENET_LEGACY)
