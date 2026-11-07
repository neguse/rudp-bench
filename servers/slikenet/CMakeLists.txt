cmake_minimum_required(VERSION 3.16)

project(rudp_bench_slikenet C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)

# SLikeNet 付属の CMake は cmake 2.6 指定で、DLL と OpenSSL も要るため使わない。
# src/crypto は実験的で OpenSSL 1.x の API を使い、接続には要らないので外す。
set(SLIKENET_DIR ../../third_party/slikenet)
file(GLOB SLIKENET_SRCS ${SLIKENET_DIR}/Source/src/*.cpp)
add_library(slikenet_vendor STATIC ${SLIKENET_SRCS})
target_include_directories(slikenet_vendor SYSTEM PUBLIC
  ${SLIKENET_DIR}/Source/include ${SLIKENET_DIR}/DependentExtensions)
# 付属の CMake と同じく retail 構成でビルドする
target_compile_definitions(slikenet_vendor PRIVATE _RETAIL)
target_compile_options(slikenet_vendor PRIVATE -w)

find_package(Threads REQUIRED)

add_executable(slikenet_sut slikenet.cc)
target_link_libraries(slikenet_sut PRIVATE benchkit slikenet_vendor Threads::Threads)
target_compile_options(slikenet_sut PRIVATE -Wall -Wextra -Wpedantic -Werror)
