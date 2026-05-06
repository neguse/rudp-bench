# UDT4 adapter — UDT 4.11 SDK (Yunhong Gu et al.) is vendored via FetchContent.
# The original SourceForge tarball is fetched at configure time. GitHub forks
# referenced by older docs (loonycyborg/UDT-fixed, eric-yhc/udt4) no longer exist.

include(FetchContent)
FetchContent_Declare(
  udt4_sdk
  URL      https://downloads.sourceforge.net/udt/udt.sdk.4.11.tar.gz
  URL_HASH MD5=30b1556e5cf0afe179e40a53a1371b08
)
FetchContent_MakeAvailable(udt4_sdk)

set(UDT4_SRC_DIR "${udt4_sdk_SOURCE_DIR}/src")
add_library(udt4_lib STATIC
  ${UDT4_SRC_DIR}/api.cpp
  ${UDT4_SRC_DIR}/buffer.cpp
  ${UDT4_SRC_DIR}/cache.cpp
  ${UDT4_SRC_DIR}/ccc.cpp
  ${UDT4_SRC_DIR}/channel.cpp
  ${UDT4_SRC_DIR}/common.cpp
  ${UDT4_SRC_DIR}/core.cpp
  ${UDT4_SRC_DIR}/epoll.cpp
  ${UDT4_SRC_DIR}/list.cpp
  ${UDT4_SRC_DIR}/md5.cpp
  ${UDT4_SRC_DIR}/packet.cpp
  ${UDT4_SRC_DIR}/queue.cpp
  ${UDT4_SRC_DIR}/window.cpp
)
target_include_directories(udt4_lib PUBLIC ${UDT4_SRC_DIR})
# UDT 4.11 uses removed throw() specs and old-style headers; silence on modern gcc/clang.
target_compile_options(udt4_lib PRIVATE -w)
target_link_libraries(udt4_lib PUBLIC pthread)

add_library(adapter_udt4 STATIC udt4_adapter.cc)
target_link_libraries(adapter_udt4 PUBLIC rudp_bench_harness udt4_lib)
target_compile_options(adapter_udt4 PRIVATE -Wno-deprecated)
