cmake_minimum_required(VERSION 3.16)

project(rudp_bench_v2_raw_udp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)

add_subdirectory(../../benchkit benchkit-build)

# raw_udp は生 UDP のみ。外部トランスポートライブラリは要らない。
# MSG_TRUNC / inet_pton 等の POSIX+BSD シンボルのため _DEFAULT_SOURCE を要求する。
add_executable(raw_udp_server server.c)
target_link_libraries(raw_udp_server PRIVATE benchkit)
target_compile_definitions(raw_udp_server PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
target_compile_options(raw_udp_server PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(raw_udp_client client.c)
target_link_libraries(raw_udp_client PRIVATE benchkit)
target_compile_definitions(raw_udp_client PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
target_compile_options(raw_udp_client PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME raw_udp_loopback_smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.py"
            "$<TARGET_FILE:raw_udp_server>" "$<TARGET_FILE:raw_udp_client>"
  )
  set_tests_properties(raw_udp_loopback_smoke PROPERTIES TIMEOUT 45)
endif()
