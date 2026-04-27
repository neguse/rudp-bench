add_library(adapter_raw_udp STATIC raw_udp_adapter.cc)
target_link_libraries(adapter_raw_udp PUBLIC rudp_bench_harness)
target_compile_options(adapter_raw_udp PRIVATE -Wall -Wextra)
