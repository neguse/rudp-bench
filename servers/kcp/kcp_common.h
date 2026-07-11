#ifndef RUDP_BENCH_KCP_COMMON_H
#define RUDP_BENCH_KCP_COMMON_H

#include "benchkit.h"
#include "ikcp.h"

// KCP は ARQ プロトコルのみ提供し、socket を持たない(README 参照)。
// wire format は先頭 1 byte で channel を区別する:
//   0x00 + benchspec payload      … loss-tolerant(KCP バイパスの素 datagram)
//   0x01 + KCP frame              … must-deliver(KCP ARQ。conv = origin_id)
// 2 channel 構成は kcp2k 等の実運用と同型で、v1 adapter から引き継ぐ。

#define KCP_PREFIX_RAW 0x00
#define KCP_PREFIX_KCP 0x01
#define KCP_PREFIX_BYTES 1u

// tuning は 2026-05-31 の sweep で選定した値(adapters/kcp の設計メモ):
// interval 5ms は高 conn 側の HoL stall 回復が勝つ、window は 256 が最良
// (大きい window は ARQ の buffering latency を足すだけ)、fastresend=2、
// nocwnd=1(Reno cwnd 無効)。socket buffer 256KB は 1MB で bufferbloat し
// delivery 0.78→0.52 に悪化した実測から。
#define KCP_MTU_BYTES 1400
#define KCP_WND 256
#define KCP_INTERVAL_MS 5
#define KCP_RESEND 2
#define KCP_NOCWND 1
#define KCP_SOCKET_BUFFER_BYTES (256 * 1024)

// waitsnd(未 ACK + 送信待ち segment 数)がこれを超えたら MD 送信を
// 受け付けない(unbounded queue の bufferbloat を防ぐ backpressure。
// 失敗 slot は未送信として metrics の分母に残る)。
#define KCP_MAX_WAITSND (KCP_WND * 4)

#define KCP_MAX_DATAGRAM_BYTES 65507u
#define KCP_MAX_RAW_PAYLOAD_BYTES (KCP_MAX_DATAGRAM_BYTES - KCP_PREFIX_BYTES)

static inline IUINT32 kcp_now_ms(void) {
  return (IUINT32)(bk_now_ns() / 1000000ull);
}

// stock ikcp は ikcp_nodelay で interval<10ms を 10ms に clamp する。
// interval は公開 field なので vendored source を変えずに直接上書きする
// (v1 adapter と同じ手法)。
static inline void kcp_apply_tuning(ikcpcb *kcp) {
  ikcp_nodelay(kcp, 1, KCP_INTERVAL_MS, KCP_RESEND, KCP_NOCWND);
  kcp->interval = (IUINT32)KCP_INTERVAL_MS;
  ikcp_setmtu(kcp, KCP_MTU_BYTES);
  ikcp_wndsize(kcp, KCP_WND, KCP_WND);
}

static inline const char *kcp_describe_json(void) {
  return "{\"transport\":\"kcp\","
         "\"class_mapping\":{"
         "\"loss_tolerant\":{\"primitive\":\"udp-datagram-sidechannel\","
         "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
         "\"realization\":\"native\"},"
         "\"must_deliver\":{\"primitive\":\"kcp-arq\","
         "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
         "\"realization\":\"native\"}},"
         "\"coalescing\":\"kcp-segment\",\"cc_algo\":\"none\","
         "\"thread_model\":\"single\",\"encryption\":false,"
         "\"payload_pattern\":\"splitmix64-v1\","
         "\"wire_compression\":\"none\","
         "\"max_payload_bytes\":65506,"
         "\"scenarios\":[\"environment_baseline\","
         "\"authoritative_state\",\"room_relay\"],"
         "\"tuning\":["
         "{\"knob\":\"ikcp_nodelay\","
         "\"value\":\"nodelay=1,interval=5ms,fastresend=2,nocwnd=1\","
         "\"upstream_ref\":\"https://github.com/skywind3000/kcp/blob/master/ikcp.h\"},"
         "{\"knob\":\"ikcp_interval_direct\","
         "\"value\":\"interval=5ms (public field bypasses the 10ms clamp)\","
         "\"upstream_ref\":\"https://github.com/skywind3000/kcp/blob/master/ikcp.c\"},"
         "{\"knob\":\"ikcp_wndsize\",\"value\":\"snd=256,rcv=256\","
         "\"upstream_ref\":\"https://github.com/skywind3000/kcp/blob/master/ikcp.h\"},"
         "{\"knob\":\"ikcp_setmtu\",\"value\":\"1400\","
         "\"upstream_ref\":\"https://github.com/skywind3000/kcp/blob/master/ikcp.h\"},"
         "{\"knob\":\"socket_buffers\",\"value\":\"256KiB\","
         "\"upstream_ref\":\"https://man7.org/linux/man-pages/man7/socket.7.html\"},"
         "{\"knob\":\"send_backpressure\",\"value\":\"max_waitsnd=1024\","
         "\"upstream_ref\":\"https://github.com/skywind3000/kcp/blob/master/ikcp.h\"}"
         "]}";
}

#endif  // RUDP_BENCH_KCP_COMMON_H
