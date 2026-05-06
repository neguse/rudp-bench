# msquic upstream: static lib, no tests/perf/tools, quictls TLS backend.
set(QUIC_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_PERF     OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_TEST     OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(QUIC_TLS_LIB        "quictls" CACHE STRING "" FORCE)
set(QUIC_ENABLE_LOGGING OFF CACHE BOOL "" FORCE)

# msquic C code uses non-standard extensions incompatible with -Wpedantic
get_directory_property(_opts COMPILE_OPTIONS)
list(REMOVE_ITEM _opts "-Wpedantic")
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_opts}")

add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/msquic msquic_build EXCLUDE_FROM_ALL)

add_library(adapter_msquic STATIC msquic_adapter.cc)
find_library(NUMA_LIB numa REQUIRED)
target_link_libraries(adapter_msquic PUBLIC rudp_bench_harness msquic ${NUMA_LIB})
target_include_directories(adapter_msquic PUBLIC ${CMAKE_SOURCE_DIR}/third_party/msquic/src/inc)
