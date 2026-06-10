# SLikeNet upstream provides RakNetLibStatic via its own CMakeLists.
# EXCLUDE_FROM_ALL: 上流コードはデフォルトビルド対象から除外する。
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/slikenet slikenet_build EXCLUDE_FROM_ALL)

# 上流コードに -Werror は付かないが、グローバルの -Wall/-Wextra/-Wpedantic が
# 伝播して大量の警告が出るため -w で全部抑制する。
target_compile_options(RakNetLibStatic PRIVATE -w)

# update thread を起こさず、adapter の poll() が RunUpdateCycle() を駆動する
# (upstream 提供のコンパイル設定)。client 側は connection ごとに RakPeer を
# 持つ構造のため、既定だと conns/プロセス 個の update thread が立ち、pinned
# client CPU 上の thread 乱立で client_tick invalid が多発していた。socket の
# blocking recv thread は無条件生成なので残る (idle 時は recvfrom でブロック)。
target_compile_definitions(RakNetLibStatic PRIVATE RAKPEER_USER_THREADED=1)

# NOTE: USE_SLIDING_WINDOW_CONGESTION_CONTROL=0 (UDT 風 rate-based CC) も試したが、
# SLikeNet v0.1.3 の CCRakNetUDT 経路は旧 RakNet の型名 (RakNetTimeMS) が残って
# おりビルド不能 (メンテされていない)。submodule にはパッチしない方針のため断念。
# 既定の CCRakNetSlidingWindow は TCP 風で netem の 1% ランダムロスをウィンドウ
# 半減で受けるため、broadcast fanout の unreliable が ~2-3% 遅延損する特性は残る。

# adapter wrapper
add_library(adapter_slikenet STATIC slikenet_adapter.cc)
target_link_libraries(adapter_slikenet PUBLIC rudp_bench_harness RakNetLibStatic)
target_include_directories(adapter_slikenet PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/slikenet/Source/include)
