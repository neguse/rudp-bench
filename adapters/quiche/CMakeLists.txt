# quiche (Cloudflare): Rust QUIC with C FFI.
# Built as cdylib (.so) via cargo to hide internal BoringSSL symbols.

find_program(CARGO_EXECUTABLE cargo REQUIRED)

set(QUICHE_SRC_DIR ${CMAKE_SOURCE_DIR}/third_party/quiche/quiche)
set(QUICHE_BUILD_DIR ${CMAKE_BINARY_DIR}/quiche_cargo)

add_custom_command(
    OUTPUT ${QUICHE_BUILD_DIR}/release/libquiche.so
    COMMAND ${CMAKE_COMMAND} -E env
        CARGO_TARGET_DIR=${QUICHE_BUILD_DIR}
        ${CARGO_EXECUTABLE} build --release --features ffi
    # cargo produces libquiche.so with SONAME libquiche.so.0; create symlink
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        libquiche.so ${QUICHE_BUILD_DIR}/release/libquiche.so.0
    WORKING_DIRECTORY ${QUICHE_SRC_DIR}
    COMMENT "Building quiche via cargo (cdylib)"
    VERBATIM
)
add_custom_target(quiche_cargo DEPENDS ${QUICHE_BUILD_DIR}/release/libquiche.so)

add_library(quiche_lib SHARED IMPORTED)
set_target_properties(quiche_lib PROPERTIES
    IMPORTED_LOCATION ${QUICHE_BUILD_DIR}/release/libquiche.so
    IMPORTED_SONAME libquiche.so.0
)
add_dependencies(quiche_lib quiche_cargo)

add_library(adapter_quiche STATIC quiche_adapter.cc)
target_link_libraries(adapter_quiche PUBLIC rudp_bench_harness quiche_lib)
target_include_directories(adapter_quiche PUBLIC ${QUICHE_SRC_DIR}/include)
