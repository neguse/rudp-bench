cmake_minimum_required(VERSION 3.16)

project(benchkit C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(benchkit
  src/clock.c
  src/control.c
  src/metrics.c
  src/payload.c
  src/plan.c
)

target_include_directories(benchkit PUBLIC include)
target_compile_definitions(benchkit PUBLIC _POSIX_C_SOURCE=200809L)
target_compile_options(benchkit PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CTest)

if(BUILD_TESTING)
  find_package(Threads REQUIRED)

  add_library(benchkit_test_fixture
    tests/fixture_transport.c
  )
  target_link_libraries(benchkit_test_fixture PUBLIC benchkit)
  target_compile_options(benchkit_test_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)

  foreach(test_name IN ITEMS
      test_payload
      test_plan
      test_metrics_zero
      test_authoritative_metrics
      test_fault_inject
      test_control)
    add_executable(${test_name} tests/${test_name}.c)
    target_link_libraries(${test_name} PRIVATE benchkit benchkit_test_fixture Threads::Threads)
    target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
