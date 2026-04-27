# SLikeNet upstream provides RakNetLibStatic via its own CMakeLists.
# EXCLUDE_FROM_ALL: 上流コードはデフォルトビルド対象から除外する。
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/slikenet slikenet_build EXCLUDE_FROM_ALL)

# 上流コードに -Werror は付かないが、グローバルの -Wall/-Wextra/-Wpedantic が
# 伝播して大量の警告が出るため -w で全部抑制する。
target_compile_options(RakNetLibStatic PRIVATE -w)

# adapter wrapper
add_library(adapter_slikenet STATIC slikenet_adapter.cc)
target_link_libraries(adapter_slikenet PUBLIC rudp_bench_harness RakNetLibStatic)
target_include_directories(adapter_slikenet PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/slikenet/Source/include)
