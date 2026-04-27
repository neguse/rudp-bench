add_library(rudp_bench_harness STATIC
  adapter_registry.cc
  scenario.cc
)
target_include_directories(rudp_bench_harness PUBLIC ${CMAKE_SOURCE_DIR})
