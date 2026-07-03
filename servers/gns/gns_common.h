// gns_common.h — rudp-bench v2 GameNetworkingSockets server/client 共有ヘルパ
#ifndef RUDP_BENCH_GNS_COMMON_H
#define RUDP_BENCH_GNS_COMMON_H

#include "benchkit.h"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace rudp_bench_gns {

// 送信パス上の実サイズ上限:
// - loss-tolerant (unreliable): k_cbMaxUnreliableMsgSizeSend = 15*1100 = 16500B
//   (third_party/gns/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.h)。
//   1 パケット(~1200B plaintext)を超える unreliable はセグメント分割され、
//   1 セグメントの loss でメッセージ全体が落ちる。
// - must-deliver (reliable): k_cbMaxSteamNetworkingSocketsMessageSizeSend = 512KB
//   (include/steam/steamnetworkingtypes.h)。
constexpr size_t kMaxPayloadLossTolerant = 15u * 1100u;
constexpr size_t kMaxPayloadMustDeliver =
    static_cast<size_t>(k_cbMaxSteamNetworkingSocketsMessageSizeSend);

inline void debug_output(ESteamNetworkingSocketsDebugOutputType type,
                         const char *msg) {
  std::fprintf(stderr, "gns[%d]: %s\n", static_cast<int>(type), msg);
}

inline void ensure_gns() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    SteamNetworkingErrMsg err;
    if (!GameNetworkingSockets_Init(nullptr, err)) {
      std::fprintf(stderr, "GameNetworkingSockets_Init failed: %s\n", err);
      std::abort();
    }
    SteamNetworkingUtils()->SetDebugOutputFunction(
        k_ESteamNetworkingSocketsDebugOutputType_Important, debug_output);
    std::atexit([]() { GameNetworkingSockets_Kill(); });
  });
}

// class → GNS 送信フラグ。
// - loss-tolerant → k_nSteamNetworkingSend_UnreliableNoNagle
//   (upstream が latency 重視の unreliable 送信向けに定義する複合フラグ)
// - must-deliver → k_nSteamNetworkingSend_Reliable(Nagle は library 既定 5ms のまま)
inline int send_flags_for(uint8_t flags) {
  return (flags & BK_FLAG_MUST_DELIVER) != 0
             ? k_nSteamNetworkingSend_Reliable
             : k_nSteamNetworkingSend_UnreliableNoNagle;
}

inline int send_payload(ISteamNetworkingSockets *iface,
                        HSteamNetConnection conn, const void *data, size_t len,
                        uint8_t flags) {
  const EResult r = iface->SendMessageToConnection(
      conn, data, static_cast<uint32>(len), send_flags_for(flags), nullptr);
  return r == k_EResultOK ? 0 : -1;
}

// 開示 metadata。cc_algo: GNS(SNP) は送信レートを SendRateMin/Max
// (library 既定はどちらも 256KB/s)に clamp する token bucket で制御する。
// v2 では既定値のまま。encryption は GNS 既定で有効(AES-256-GCM +
// curve25519 鍵交換)。max_payload_bytes は両 class 共通で使える上限 =
// unreliable 送信上限 16500B(reliable 単体は 512KB まで — README 参照)。
inline void print_describe() {
  std::puts(
      "{\"transport\":\"gns\","
      "\"class_mapping\":{\"loss_tolerant\":\"unreliable-no-nagle\","
      "\"must_deliver\":\"reliable\"},"
      "\"coalescing\":\"none\","
      "\"cc_algo\":\"token-bucket-fixed-rate-256KBps\","
      "\"thread_model\":\"internal_worker\","
      "\"encryption\":true,"
      "\"max_payload_bytes\":16500,"
      "\"tuning\":["
      "{\"knob\":\"_CERT compile definition\","
      "\"value\":\"defined\","
      "\"upstream_ref\":\"Valve retail build flag: disables DBGFLAG_ASSERT "
      "(third_party/gns/src/public/tier0/dbg.h: 'it should be legal to turn "
      "this off') and sets STEAMNETWORKINGSOCKETS_SNP_PARANOIA=0 "
      "(third_party/gns/src/steamnetworkingsockets/"
      "steamnetworkingsockets_internal.h)\"}"
      "]}");
}

inline uint64_t add_ns(uint64_t a, uint64_t b) {
  return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

}  // namespace rudp_bench_gns

#endif  // RUDP_BENCH_GNS_COMMON_H
