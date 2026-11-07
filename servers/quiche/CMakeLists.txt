cmake_minimum_required(VERSION 3.16)

project(rudp_bench_quiche C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(../../benchkit benchkit-build)

# C API は ffi feature でだけ作られる(quiche/README.md の Calling quiche from C/C++)
set(QUICHE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/quiche/quiche)
set(QUICHE_LIB ${CMAKE_CURRENT_BINARY_DIR}/cargo/release/libquiche.a)
add_custom_target(quiche_cargo
  COMMAND cargo build --release --features ffi
          --manifest-path ${QUICHE_DIR}/Cargo.toml
          --target-dir ${CMAKE_CURRENT_BINARY_DIR}/cargo
  BYPRODUCTS ${QUICHE_LIB}
  USES_TERMINAL)

find_package(Threads REQUIRED)

# quiche_drop_old_dgram_sut は LT の datagram を送信待ちに足す前に古いものを捨てる(quiche.c)
foreach(target IN ITEMS quiche_sut quiche_drop_old_dgram_sut)
  add_executable(${target} quiche.c)
  add_dependencies(${target} quiche_cargo)
  target_include_directories(${target} SYSTEM PRIVATE ${QUICHE_DIR}/include)
  # libquiche.a に入る BoringSSL が libstdc++ を使う
  target_link_libraries(${target} PRIVATE benchkit ${QUICHE_LIB} stdc++ Threads::Threads ${CMAKE_DL_LIBS} m)
  target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE
    QUICHE_EXAMPLES_DIR="${QUICHE_DIR}/examples")
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
target_compile_definitions(quiche_drop_old_dgram_sut PRIVATE QUICHE_DROP_OLD_DGRAM)
