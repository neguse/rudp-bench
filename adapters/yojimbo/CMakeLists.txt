# yojimbo adapter — build yojimbo sources directly via CMake (no premake5 required).
# premake5.lua の依存関係を直接 CMakeLists に写したもの。
# On Linux: system libsodium is used (<sodium.h> from libsodium-dev).
# sodium/ bundled directory (Windows only) is NOT included.

set(YOJIMBO_DIR ${CMAKE_SOURCE_DIR}/third_party/yojimbo)

# --- tlsf allocator (C) ---
add_library(yojimbo_tlsf STATIC ${YOJIMBO_DIR}/tlsf/tlsf.c)
target_include_directories(yojimbo_tlsf PUBLIC ${YOJIMBO_DIR}/tlsf)
target_compile_options(yojimbo_tlsf PRIVATE -w)

# --- reliable layer (C) ---
add_library(yojimbo_reliable STATIC ${YOJIMBO_DIR}/reliable/reliable.c)
target_include_directories(yojimbo_reliable PUBLIC ${YOJIMBO_DIR}/reliable)
target_compile_options(yojimbo_reliable PRIVATE -w)

# --- netcode (C, needs libsodium) ---
find_library(SODIUM_LIB sodium REQUIRED)
add_library(yojimbo_netcode STATIC ${YOJIMBO_DIR}/netcode/netcode.c)
target_include_directories(yojimbo_netcode PUBLIC ${YOJIMBO_DIR}/netcode)
target_compile_options(yojimbo_netcode PRIVATE -w)
target_link_libraries(yojimbo_netcode PUBLIC ${SODIUM_LIB})

# --- yojimbo C++ library ---
# premake5.lua の includedirs { ".", "include", "serialize", "netcode", "reliable", "tlsf" } に対応。
# "${YOJIMBO_DIR}" は "." に相当し、"tlsf/tlsf.h" など サブディレクトリ相対パスの解決に必要。
file(GLOB YOJIMBO_CXX_SOURCES ${YOJIMBO_DIR}/source/*.cpp)
add_library(yojimbo_lib STATIC ${YOJIMBO_CXX_SOURCES})
target_include_directories(yojimbo_lib PUBLIC
    ${YOJIMBO_DIR}
    ${YOJIMBO_DIR}/include
    ${YOJIMBO_DIR}/serialize
    ${YOJIMBO_DIR}/netcode
    ${YOJIMBO_DIR}/reliable
    ${YOJIMBO_DIR}/tlsf
)
target_compile_options(yojimbo_lib PRIVATE -w)
target_link_libraries(yojimbo_lib PUBLIC
    yojimbo_netcode
    yojimbo_reliable
    yojimbo_tlsf
    ${SODIUM_LIB}
)

# --- adapter wrapper ---
add_library(adapter_yojimbo STATIC yojimbo_adapter.cc)
target_link_libraries(adapter_yojimbo PUBLIC rudp_bench_harness yojimbo_lib)
target_include_directories(adapter_yojimbo PUBLIC
    ${YOJIMBO_DIR}/include
    ${YOJIMBO_DIR}/serialize
)
# Suppress upstream header warnings leaking into our TU.
target_compile_options(adapter_yojimbo PRIVATE
    -Wno-pedantic -Wno-unused-parameter -Wno-missing-field-initializers
)
