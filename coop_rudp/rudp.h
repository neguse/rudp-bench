#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rudp_endpoint rudp_endpoint;
typedef struct rudp_conn rudp_conn;

typedef struct rudp_addr {
  uint8_t data[128];
  uint32_t len;
} rudp_addr;

typedef struct rudp_out_packet {
  rudp_addr addr;
  const uint8_t* data;
  size_t len;
} rudp_out_packet;

typedef struct rudp_in_packet {
  rudp_addr addr;
  uint8_t* data;
  size_t cap;
  size_t len;
} rudp_in_packet;

typedef struct rudp_socket_vtable {
  void* user;
  int (*send_batch)(void* user, const rudp_out_packet* packets, size_t count);
  int (*recv_batch)(void* user, rudp_in_packet* packets, size_t max_count);
  uint64_t (*now_ns)(void* user);
} rudp_socket_vtable;

/* ワイヤフォーマットの固定長。adapter などの呼び出し側が独自に二重定義せず
   この定数を参照する（コアの format 変更をサイレントに壊さないため）。 */
#define RUDP_WIRE_PACKET_HEADER_BYTES 32u
#define RUDP_WIRE_DATA_FRAME_HEADER_BYTES 20u

typedef struct rudp_endpoint_config {
  rudp_socket_vtable socket;
  uint16_t mtu;
  uint16_t max_payload_bytes;
  uint32_t max_conns;
  uint32_t max_flows;
  uint32_t max_channels;
  uint32_t max_messages;
  uint32_t max_recv_events;
  uint32_t max_ordered_holds;
  uint32_t sent_packet_count;
  uint32_t recv_batch_size;
  uint32_t send_batch_size;
  /* RTT サンプルが無いときの初期 RTO。サンプル取得後は SRTT+4*RTTVAR による
     適応 RTO（min_rto_ms..max_rto_ms でクランプ）が使われる。 */
  uint32_t rto_ms;
  /* 適応 RTO の下限/上限。0 なら既定（20ms / 10000ms）。 */
  uint32_t min_rto_ms;
  uint32_t max_rto_ms;
  /* ピアから何も受信しないまま RTO 再送ラウンドが何回続いたら abort するか。
     0 なら無制限。（パケット数ではなくラウンド数で数える） */
  uint32_t max_retransmits;
  uint32_t idle_timeout_ms;
  uint32_t initial_safe_bps;
  /* 受信パケット起点の新規コネクション生成を 1 poll あたり何個まで許すか。
     0 なら無制限。ハンドシェイク/cookie を持たないため、spoof された
     conn_id による conn テーブル枯渇の緩和はこのレート制限のみ。 */
  uint32_t max_incoming_conns_per_poll;
  /* フラグメント再組立スロット数。0 なら max_recv_events と同数（互換既定）。
     単一フレーム運用の呼び出し側は小さくしてメモリを節約できる。 */
  uint32_t max_reassemblies;
  uint8_t skip_unreliable_acks;
} rudp_endpoint_config;

int rudp_endpoint_create(rudp_endpoint** out, const rudp_endpoint_config* config);
void rudp_endpoint_destroy(rudp_endpoint* ep);

void rudp_endpoint_poll(rudp_endpoint* ep, uint64_t now_ns);
void rudp_endpoint_flush(rudp_endpoint* ep, uint64_t now_ns);

int rudp_endpoint_connect(
    rudp_endpoint* ep,
    const rudp_addr* peer,
    uint64_t conn_id,
    rudp_conn** out);

rudp_conn* rudp_endpoint_find_conn(rudp_endpoint* ep, uint64_t conn_id);
uint64_t rudp_conn_id(const rudp_conn* conn);
void rudp_conn_abort(rudp_conn* conn);

/* コアが受理しうるパケットかの事前判定（magic/長さ/ヘッダ形状のみ）。
   呼び出し側がワイヤフォーマットを二重定義せずに demux できるようにする。
   有効なら 1 を返し、out_conn_id が非 NULL ならヘッダの conn_id を書く。 */
int rudp_packet_precheck(const void* data, size_t len, uint16_t mtu,
                         uint64_t* out_conn_id);

typedef struct rudp_status {
  uint32_t safe_bps;
  uint32_t pacing_bps;

  uint32_t rtt_ms;
  uint32_t min_rtt_ms;
  uint32_t queue_delay_ms;

  uint32_t loss_ppm;
  uint32_t reorder_ppm;

  uint32_t inflight_bytes;
  uint32_t send_queue_bytes;
  uint32_t retransmit_queue_bytes;

  uint32_t mtu;
  uint8_t usable;
} rudp_status;

void rudp_get_status(rudp_conn* conn, rudp_status* out);

typedef struct rudp_flow_limit {
  uint16_t flow_id;

  uint32_t max_bps;
  uint32_t max_queue_bytes;
  uint32_t max_delay_ms;

  uint8_t priority;
} rudp_flow_limit;

void rudp_set_flow_limits(
    rudp_conn* conn,
    const rudp_flow_limit* limits,
    size_t count);

typedef enum rudp_reliability {
  RUDP_UNRELIABLE,
  RUDP_UNRELIABLE_SEQUENCED,
  RUDP_RELIABLE_UNORDERED,
  RUDP_RELIABLE_ORDERED
} rudp_reliability;

typedef struct rudp_send_opts {
  uint16_t flow_id;
  uint16_t channel_id;

  uint8_t priority;
  rudp_reliability reliability;

  uint32_t deadline_ms;
  uint32_t replace_key;
} rudp_send_opts;

typedef enum rudp_send_result {
  RUDP_SEND_OK,
  RUDP_SEND_QUEUED,
  RUDP_SEND_DROPPED,
  RUDP_SEND_RATE_LIMITED,
  RUDP_SEND_QUEUE_FULL,
  RUDP_SEND_EXPIRED,
  RUDP_SEND_UNUSABLE
} rudp_send_result;

rudp_send_result rudp_send(
    rudp_conn* conn,
    const void* data,
    size_t len,
    const rudp_send_opts* opts);

typedef struct rudp_flow_stats {
  uint16_t flow_id;

  uint32_t target_bps;
  uint32_t sent_bps;
  uint32_t delivered_bps;
  uint32_t dropped_bps;

  uint32_t queued_bytes;
  uint32_t queue_delay_ms;

  uint32_t send_ok;
  uint32_t send_dropped;
  uint32_t send_rate_limited;
  uint32_t send_queue_full;
  uint32_t expired;
  uint32_t retransmits;
} rudp_flow_stats;

size_t rudp_get_flow_stats(
    rudp_conn* conn,
    rudp_flow_stats* out,
    size_t max_count);

typedef struct rudp_recv_info {
  rudp_conn* conn;
  uint64_t conn_id;
  uint16_t flow_id;
  uint16_t channel_id;
  rudp_reliability reliability;
} rudp_recv_info;

// Returns 1 when an event is copied, 0 when no event is queued, and -1 for
// invalid arguments or a too-small destination buffer. On a too-small buffer,
// the event is preserved and out_len/out_info describe the queued event.
int rudp_recv(
    rudp_endpoint* ep,
    void* data,
    size_t cap,
    size_t* out_len,
    rudp_recv_info* out_info);

// Returns 1 when an event is borrowed, 0 when no event is queued, and -1 for
// invalid required arguments. The borrowed pointer remains valid until the next
// receive/borrow call on the endpoint.
// 注意: 現実装は「ゼロコピー借用」ではない。イベントは endpoint 内の安定した
// staging バッファへ 1 回コピーされ、そのポインタを返す（呼び出し側コピーを
// 1 回省くだけの API）。
int rudp_recv_borrow(
    rudp_endpoint* ep,
    const void** data,
    size_t* out_len,
    rudp_recv_info* out_info);

// Compatibility helper for callers that only need conn_id and reliability.
int rudp_recv_borrow_meta(
    rudp_endpoint* ep,
    const void** data,
    size_t* out_len,
    uint64_t* out_conn_id,
    rudp_reliability* out_reliability);

#ifdef __cplusplus
}
#endif
