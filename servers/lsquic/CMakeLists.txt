cmake_minimum_required(VERSION 3.16)

project(rudp_bench_lsquic C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)

add_subdirectory(../../third_party/boringssl boringssl-build EXCLUDE_FROM_ALL)

# BoringSSL と lsquic をともに add_subdirectory で入れるときの指定(lsquic の CMakeLists.txt)
set(LSQUIC_BIN OFF CACHE BOOL "" FORCE)
set(LSQUIC_TESTS OFF CACHE BOOL "" FORCE)
set(BORINGSSL_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boringssl/include)
set(LIBSSL_LIB_ssl ssl)
set(LIBSSL_LIB_crypto crypto)
add_subdirectory(../../third_party/lsquic lsquic-build EXCLUDE_FROM_ALL)

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_executable(lsquic_sut lsquic.c)
target_link_libraries(lsquic_sut PRIVATE benchkit lsquic ssl crypto ZLIB::ZLIB Threads::Threads m)
target_include_directories(lsquic_sut SYSTEM PRIVATE
  ../../third_party/lsquic/include ../../third_party/boringssl/include)
target_compile_definitions(lsquic_sut PRIVATE _DEFAULT_SOURCE)
target_compile_options(lsquic_sut PRIVATE -Wall -Wextra -Wpedantic -Werror)
