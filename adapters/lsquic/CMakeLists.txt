# lsquic (LiteSpeed): C-based QUIC.
# Built as a shared library with version script to hide BoringSSL symbols.

set(BORINGSSL_SRC ${CMAKE_SOURCE_DIR}/third_party/boringssl)
set(LSQUIC_SRC ${CMAKE_SOURCE_DIR}/third_party/lsquic)

# Everything built here ends up in a shared library — need PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- BoringSSL (static, consumed inside the .so) ---
# BoringSSL's cmake uses C++; silence warnings inherited from our top-level flags.
get_directory_property(_parent_opts COMPILE_OPTIONS)
list(REMOVE_ITEM _parent_opts "-Wpedantic" "-Wextra")
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_parent_opts}")

add_subdirectory(${BORINGSSL_SRC} ${CMAKE_BINARY_DIR}/boringssl_build EXCLUDE_FROM_ALL)

# Restore top-level options for our own code
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_parent_opts}")

# --- lsquic (static target, then wrapped into .so) ---
set(LSQUIC_BIN   OFF CACHE BOOL "" FORCE)
set(LSQUIC_TESTS OFF CACHE BOOL "" FORCE)
set(LSQUIC_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BORINGSSL_DIR      ${BORINGSSL_SRC} CACHE PATH "" FORCE)
set(BORINGSSL_INCLUDE  ${BORINGSSL_SRC}/include CACHE PATH "" FORCE)
# Tell lsquic to use BoringSSL CMake targets directly
set(LIBSSL_LIB_ssl     ssl   CACHE STRING "" FORCE)
set(LIBSSL_LIB_crypto  crypto CACHE STRING "" FORCE)

add_subdirectory(${LSQUIC_SRC} ${CMAKE_BINARY_DIR}/lsquic_build EXCLUDE_FROM_ALL)

# --- Shared library wrapper (hides BoringSSL symbols) ---
add_library(lsquic_shared SHARED
    lsquic_cert_helper.c
)
target_include_directories(lsquic_shared PRIVATE
    ${BORINGSSL_SRC}/include
    ${LSQUIC_SRC}/include
)
target_link_libraries(lsquic_shared PRIVATE
    -Wl,--whole-archive
    lsquic
    -Wl,--no-whole-archive
    ssl
    crypto
    z
    pthread
)
set_target_properties(lsquic_shared PROPERTIES
    OUTPUT_NAME lsquic_bundle
    LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/lsquic_exports.map"
)

# --- Adapter (static lib linking against the .so) ---
add_library(adapter_lsquic STATIC lsquic_adapter.cc)
target_link_libraries(adapter_lsquic PUBLIC rudp_bench_harness lsquic_shared)
target_include_directories(adapter_lsquic PUBLIC
    ${LSQUIC_SRC}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
