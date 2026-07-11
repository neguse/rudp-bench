cmake_minimum_required(VERSION 3.16)

project(rudp_bench_v2_msquic C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_subdirectory(../../benchkit benchkit-build)

# Match the v1 adapter's vendored MsQuic configuration: static library,
# no tests/perf/tools, quictls TLS backend.
set(QUIC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_PERF OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(QUIC_TLS_LIB "quictls" CACHE STRING "" FORCE)
set(QUIC_ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/msquic msquic-build EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)
find_library(NUMA_LIB numa REQUIRED)

add_library(msquic_bench_common STATIC msquic_common.cc)
target_link_libraries(msquic_bench_common PUBLIC msquic ${NUMA_LIB} Threads::Threads)
target_include_directories(msquic_bench_common
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(msquic_bench_common
  SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/msquic/src/inc"
)
target_compile_definitions(msquic_bench_common PUBLIC QUIC_API_ENABLE_PREVIEW_FEATURES=1)
target_compile_options(msquic_bench_common PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(msquic_server server.cc)
target_link_libraries(msquic_server PRIVATE benchkit msquic_bench_common)
target_compile_definitions(msquic_server PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(msquic_server PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(msquic_client client.cc)
target_link_libraries(msquic_client PRIVATE benchkit msquic_bench_common)
target_compile_definitions(msquic_client PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(msquic_client PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME msquic_loopback_smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../raw_udp/smoke_test.py"
            "$<TARGET_FILE:msquic_server>" "$<TARGET_FILE:msquic_client>"
  )
  set_tests_properties(msquic_loopback_smoke PROPERTIES TIMEOUT 90)
endif()
