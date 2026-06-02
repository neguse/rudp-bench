add_library(rudp_bench_harness STATIC
  adapter_registry.cc
  scenario.cc
  metrics.cc
  csv_writer.cc
  proc_sampler.cc
  runner.cc
)
target_include_directories(rudp_bench_harness PUBLIC ${CMAKE_SOURCE_DIR})

add_executable(rudp-bench main.cc)
target_link_libraries(rudp-bench PRIVATE
  rudp_bench_harness
  adapter_raw_udp
  adapter_mini_rudp
  adapter_apex_rudp
  adapter_enet
  adapter_kcp
  adapter_slikenet
  adapter_udt4
  adapter_yojimbo
  adapter_gns
  adapter_msquic
)
