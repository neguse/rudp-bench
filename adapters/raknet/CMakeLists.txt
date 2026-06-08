add_library(adapter_raknet STATIC raknet_adapter.cc)
target_link_libraries(adapter_raknet PUBLIC rudp_bench_harness RakNetLibStatic)
target_include_directories(adapter_raknet SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/slikenet/Source/include)
