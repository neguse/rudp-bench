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

# yojimbo_timeout30_sut は切断までの時間を 30 秒にする(yojimbo.cc)
foreach(target IN ITEMS yojimbo_sut yojimbo_timeout30_sut)
  add_executable(${target} yojimbo.cc)
  target_link_libraries(${target} PRIVATE benchkit yojimbo)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
target_compile_definitions(yojimbo_timeout30_sut PRIVATE YOJIMBO_TIMEOUT30)
