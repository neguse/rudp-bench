cmake_minimum_required(VERSION 3.16)

project(rudp_bench_v2_kcp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)

add_subdirectory(../../benchkit benchkit-build)

# vendored ikcp(skywind3000/kcp)。単一ファイル C ライブラリ。
# vendor code には -Werror を課さない。
add_library(kcp_vendor STATIC ../../third_party/kcp/ikcp.c)
target_include_directories(kcp_vendor PUBLIC ../../third_party/kcp)

add_executable(kcp_server server.c)
target_link_libraries(kcp_server PRIVATE benchkit kcp_vendor)
target_compile_definitions(kcp_server PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
target_compile_options(kcp_server PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(kcp_client client.c)
target_link_libraries(kcp_client PRIVATE benchkit kcp_vendor)
target_compile_definitions(kcp_client PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
target_compile_options(kcp_client PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME kcp_loopback_smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.py"
            "$<TARGET_FILE:kcp_server>" "$<TARGET_FILE:kcp_client>"
  )
  set_tests_properties(kcp_loopback_smoke PROPERTIES TIMEOUT 90)
endif()
