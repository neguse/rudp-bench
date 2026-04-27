# ENet upstream provides STATIC `enet` target via its own CMakeLists.
# EXCLUDE_FROM_ALL: 上流コードはデフォルトビルド対象から除外する。
# 上流は非MSVC環境で -Wno-error を自前で設定するため警告はエラーにならない。
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/enet enet_build EXCLUDE_FROM_ALL)

# adapter wrapper
add_library(adapter_enet STATIC enet_adapter.cc)
target_link_libraries(adapter_enet PUBLIC rudp_bench_harness enet)
target_include_directories(adapter_enet PUBLIC ${CMAKE_SOURCE_DIR}/third_party/enet/include)
