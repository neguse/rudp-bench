# KCP upstream provides STATIC `kcp` target via its own CMakeLists.
# BUILD_TESTING=OFF: upstream test binary は不要。
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/kcp kcp_build EXCLUDE_FROM_ALL)

# adapter wrapper
add_library(adapter_kcp STATIC kcp_adapter.cc)
target_link_libraries(adapter_kcp PUBLIC rudp_bench_harness kcp)
target_include_directories(adapter_kcp PUBLIC ${CMAKE_SOURCE_DIR}/third_party/kcp)
