cmake_minimum_required(VERSION 3.20)
project(rudp-bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_subdirectory(harness)
add_subdirectory(adapters/raw_udp)
add_subdirectory(adapters/mini_rudp)
add_subdirectory(adapters/enet)
add_subdirectory(adapters/kcp)
add_subdirectory(adapters/slikenet)
add_subdirectory(tests)
