cmake_minimum_required(VERSION 3.16)

project(rudp_bench_v2_enet C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)

add_subdirectory(../../benchkit benchkit-build)
add_subdirectory(../../third_party/enet enet-build)
target_compile_definitions(enet PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)

set(ENET_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/enet/include")

add_executable(enet_server server.c)
target_link_libraries(enet_server PRIVATE benchkit enet)
target_include_directories(enet_server SYSTEM PRIVATE "${ENET_INCLUDE_DIR}")
target_compile_definitions(enet_server PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(enet_server PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(enet_client client.c)
target_link_libraries(enet_client PRIVATE benchkit enet)
target_include_directories(enet_client SYSTEM PRIVATE "${ENET_INCLUDE_DIR}")
target_compile_definitions(enet_client PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(enet_client PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME enet_loopback_smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.py"
            "$<TARGET_FILE:enet_server>" "$<TARGET_FILE:enet_client>"
  )
  set_tests_properties(enet_loopback_smoke PROPERTIES TIMEOUT 45)
endif()
