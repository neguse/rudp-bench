#include "coop_rudp/rudp.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define RUDP_DEFAULT_MTU 1200u
#define RUDP_DEFAULT_MAX_CONNS 64u
#define RUDP_DEFAULT_MAX_FLOWS 64u
#define RUDP_DEFAULT_MAX_CHANNELS 64u
#define RUDP_DEFAULT_MAX_MESSAGES 4096u
#define RUDP_DEFAULT_MAX_RECV_EVENTS 1024u
#define RUDP_DEFAULT_MAX_ORDERED_HOLDS 1024u
#define RUDP_DEFAULT_SENT_PACKETS 4096u
#define RUDP_DEFAULT_RECV_BATCH 64u
#define RUDP_DEFAULT_SEND_BATCH 64u
#define RUDP_DEFAULT_RTO_MS 100u

#define RUDP_PACKET_FLAG_ACK_ONLY 0x01u
#define RUDP_PACKET_MAGIC_MASK 0xffffff00u
#define RUDP_PACKET_MAGIC 0x52554400u
#define RUDP_PACKET_VERSION_FLAGS(f) (RUDP_PACKET_MAGIC | ((uint32_t)(f) & 0xffu))

#define RUDP_FRAME_DATA 1u
#define RUDP_F_RELIABLE 0x01u
#define RUDP_F_ORDERED 0x02u
#define RUDP_F_SEQUENCED 0x04u

/* ワイヤ定数は公開ヘッダ（rudp.h）の値が単一の真実源。 */
#define RUDP_PACKET_HEADER_BYTES RUDP_WIRE_PACKET_HEADER_BYTES
#define RUDP_DATA_FRAME_HEADER_BYTES RUDP_WIRE_DATA_FRAME_HEADER_BYTES
#define RUDP_INVALID_INDEX UINT32_MAX
#define RUDP_MAX_REFS_PER_PACKET 16u
#define RUDP_MAX_FRAGS_PER_MSG 256u
#define RUDP_MAX_LOST_ACK_SEQS 4u
/* reliable msg_id 重複排除のスライディングウィンドウ幅（bit 数）。
   64bit ワード × 1024 = 65536 msg_id。ウィンドウ内は取りこぼしゼロの厳密
   判定、ウィンドウより古い msg_id は「配信済みとみなす」（重複抑止側に倒す）。
   旧実装の 4-way set-associative は衝突退避で任意レートでも重複配信が
   起きえたが、この方式は 65536 個より古い遅延再送のみが偽陽性になる。 */
#define RUDP_RX_DEDUP_WORDS 1024u
#define RUDP_RX_DEDUP_WINDOW (RUDP_RX_DEDUP_WORDS * 64u)
#define RUDP_REASM_BITMAP_WORDS (RUDP_MAX_FRAGS_PER_MSG / 64u)
#define RUDP_MAX_FLOW_COUNT 65536u
#define RUDP_MAX_CHANNEL_COUNT 65536u
#define RUDP_LOSS_PACKET_THRESHOLD 3u
#define RUDP_NS_PER_SEC 1000000000ull
#define RUDP_NS_PER_MS 1000000ull

#define RUDP_QUEUE_NONE 0u
#define RUDP_QUEUE_UNRELIABLE 1u
#define RUDP_QUEUE_RELIABLE 2u
#define RUDP_QUEUE_RETRY 3u

typedef struct rudp_ring {
  uint32_t* items;
  uint32_t cap;
  uint32_t head;
  uint32_t tail;
  uint32_t count;
} rudp_ring;

typedef struct rudp_conn_map_entry {
  uint64_t conn_id;
  uint32_t conn_index;
  uint8_t used;
} rudp_conn_map_entry;

typedef struct rudp_msg {
  uint8_t used;
  uint8_t in_retry;
  uint8_t queued_kind;
  uint8_t priority;
  uint8_t lost_ack_seq_count;
  uint16_t inflight_refs;
  rudp_reliability reliability;
  uint16_t flow_id;
  uint16_t channel_id;
  uint16_t channel_seq;
  uint16_t frag_index;
  uint16_t frag_count;
  uint32_t msg_id;
  uint32_t replace_key;
  uint32_t owner_conn_p1; /* conn index + 1; 0 = unowned */
  /* この msg を現在運んでいる in-flight パケットの seq（0 = なし）。
     msg は同時に 1 パケットにしか載らない（再送は旧パケットの loss 処理後に
     新パケットで運ばれる）ので、逆参照は単一値で足りる。 */
  uint32_t carrier_seq;
  uint32_t lost_ack_seqs[RUDP_MAX_LOST_ACK_SEQS];
  uint64_t enqueue_ns;
  uint64_t deadline_ns;
  uint16_t len;
  uint8_t* data;
} rudp_msg;

typedef struct rudp_flow_state {
  rudp_flow_limit limit;
  rudp_flow_stats stats;
  rudp_ring unreliable_q;
  rudp_ring reliable_q;
  rudp_ring retry_q;
  uint64_t tokens_bytes;
  uint64_t last_refill_ns;
  uint32_t queued_bytes;
  uint32_t deficit;
  uint32_t quantum;
} rudp_flow_state;

typedef struct rudp_ack_tracker {
  uint32_t max_seq;
  uint64_t bits;
} rudp_ack_tracker;

typedef struct rudp_channel_state {
  uint16_t next_ordered_seq;
  uint16_t expected_ordered;
} rudp_channel_state;

typedef struct rudp_sequenced_state {
  uint16_t next_send_seq;
  uint16_t latest_sequenced;
  uint8_t have_latest_sequenced;
} rudp_sequenced_state;

typedef struct rudp_sent_packet {
  uint8_t used;
  uint8_t lost;
  uint32_t conn_index;
  uint32_t seq;
  uint64_t sent_ns;
  uint16_t bytes;
  uint16_t ref_count;
  uint32_t refs[RUDP_MAX_REFS_PER_PACKET];
} rudp_sent_packet;

typedef struct rudp_ordered_hold {
  uint8_t used;
  uint32_t conn_index;
  uint32_t msg_id;
  uint16_t flow_id;
  uint16_t channel_id;
  uint16_t channel_seq;
  uint16_t len;
  rudp_reliability reliability;
  uint8_t* data;
} rudp_ordered_hold;

typedef struct rudp_recv_event_i {
  uint8_t used;
  uint8_t borrowed;
  uint32_t conn_index;
  uint64_t conn_id;
  uint16_t flow_id;
  uint16_t channel_id;
  uint16_t len;
  rudp_reliability reliability;
  uint8_t* data;
} rudp_recv_event_i;

typedef struct rudp_rx_reasm {
  uint8_t used;
  uint32_t conn_index;
  uint32_t msg_id;
  uint64_t deadline_ns;
  uint16_t flow_id;
  uint16_t channel_id;
  uint16_t channel_seq;
  uint16_t frag_count;
  uint16_t received_count;
  uint16_t total_len;
  rudp_reliability reliability;
  uint64_t received[RUDP_REASM_BITMAP_WORDS];
  uint8_t* data;
} rudp_rx_reasm;

struct rudp_conn {
  rudp_endpoint* ep;
  uint32_t index;
  uint8_t active;
  rudp_addr peer;
  uint64_t conn_id;
  uint32_t next_packet_seq;
  uint32_t next_msg_id;
  rudp_ack_tracker ack;
  uint8_t ack_dirty;
  uint32_t flow_cursor;
  rudp_flow_state* flows;
  rudp_channel_state* channels;
  rudp_sequenced_state* sequenced;
  /* msg_id ブロック（msg_id/64）でタグ付けしたリング状ビットマップ。
     rx_dedup_block[w] がブロック番号一致のときだけ rx_dedup_bits[w] が有効。 */
  uint64_t rx_dedup_bits[RUDP_RX_DEDUP_WORDS];
  uint32_t rx_dedup_block[RUDP_RX_DEDUP_WORDS];
  uint32_t rx_dedup_max; /* 観測した最大 msg_id（0 = 未観測） */
  uint64_t next_retx_ns;
  uint64_t last_rx_ns;
  uint64_t last_tx_ns;
  uint64_t srtt_ns;
  uint64_t rttvar_ns;
  uint64_t min_rtt_ns;
  uint64_t acked_packets;
  uint64_t lost_packets;
  /* ピアから何も受信しないまま経過した RTO 再送ラウンド数。パケット単位で
     数えると 1 ラウンドの一括ロスで閾値を突き抜けて正常接続を誤 abort する。 */
  uint32_t retx_rounds_since_rx;
  /* RTO 指数バックオフ段数（受信があるとリセット）。 */
  uint32_t rto_backoff;
  /* loss 応答（safe_bps 半減）を 1 ロスラウンドにつき 1 回に抑えるための
     直近半減時刻。パケット毎に半減すると一括ロスでレートが床まで落ちる。 */
  uint64_t last_rate_halve_ns;
  uint64_t recv_packets;
  uint64_t reordered_packets;
  uint32_t inflight_bytes;
  uint32_t send_queue_bytes;
  uint32_t retransmit_queue_bytes;
  uint32_t safe_bps;
  uint32_t pacing_bps;
  /* pacing_bps を実際の送信ゲートにするためのトークンバケット。
     pacing_bps == UINT32_MAX（既定）の間は無制限で、loss で safe_bps が
     半減して有限になると flush がこのトークンで送出を絞る。 */
  uint64_t pace_tokens_bytes;
  uint64_t pace_refill_ns;
  uint32_t queue_delay_ms;
  uint32_t sent_seq_head;
  uint32_t sent_seq_tail;
  uint32_t sent_seq_count;
  /* この flush で発行した最初の reliable seq（record 前のウィンドウ計算用）。
     flush の conn 処理開始時にリセットされる。 */
  uint32_t batch_reliable_first_seq;
  /* live messages owned by this conn with a recorded lost packet seq; gates
     the late-ack rescue scans in release_late_acked_messages */
  uint32_t late_ack_msg_count;
};

struct rudp_endpoint {
  rudp_endpoint_config cfg;
  uint16_t mtu;
  uint16_t max_payload;
  uint16_t frame_payload;
  uint64_t now_ns;

  rudp_conn* conns;
  rudp_flow_state* flow_storage;
  rudp_channel_state* channel_storage;
  rudp_sequenced_state* sequenced_storage;
  uint32_t* ring_storage;
  uint32_t per_conn_queue_cap;
  uint32_t ring_storage_stride;
  rudp_conn_map_entry* conn_map;
  uint32_t conn_map_cap;

  rudp_msg* msgs;
  uint8_t* msg_data;
  uint32_t* free_msgs;
  uint32_t free_msg_count;

  rudp_sent_packet* sent_packets;
  uint32_t* sent_seq_queue;
  rudp_ordered_hold* holds;
  uint8_t* hold_data;
  uint32_t ordered_hold_count; /* occupied holds; gates drain_ordered scans */

  rudp_recv_event_i* recv_events;
  uint8_t* recv_event_data;
  uint8_t* borrow_data;
  uint32_t recv_head;
  uint32_t recv_tail;
  uint32_t recv_count;

  rudp_rx_reasm* reasm;
  uint8_t* reasm_data;
  uint32_t reasm_count;

  /* この poll 内で受信パケット起点に生成した conn 数（レート制限用）。 */
  uint32_t incoming_conns_this_poll;

  uint8_t* recv_batch_data;
  rudp_in_packet* recv_batch;
  uint8_t* send_batch_data;
  rudp_out_packet* send_batch;
  uint32_t* send_batch_conn;
  uint32_t* send_batch_refs;
  uint32_t* send_batch_msgs;
  uint8_t* send_batch_msg_sources;
  uint16_t* send_batch_ref_counts;
  uint16_t* send_batch_msg_counts;
  uint8_t* send_batch_ack_only;
  uint32_t send_count;
};

static uint16_t cfg_u16(uint16_t v, uint16_t d) { return v ? v : d; }
static uint32_t cfg_u32(uint32_t v, uint32_t d) { return v ? v : d; }

static uint32_t next_pow2_u32(uint32_t v) {
  if (v <= 1u) return 1u;
  --v;
  v |= v >> 1u;
  v |= v >> 2u;
  v |= v >> 4u;
  v |= v >> 8u;
  v |= v >> 16u;
  return v + 1u;
}

static int checked_mul_size(size_t a, size_t b, size_t* out) {
  if (a != 0 && b > SIZE_MAX / a) return 0;
  *out = a * b;
  return 1;
}

static void add_u32_saturating(uint32_t* value, uint32_t add) {
  *value = (*value > UINT32_MAX - add) ? UINT32_MAX : *value + add;
}

static int seq16_after(uint16_t a, uint16_t b) {
  return a != b && (uint16_t)(a - b) < 0x8000u;
}

static int seq32_after(uint32_t a, uint32_t b) {
  return a != b && (uint32_t)(a - b) < 0x80000000u;
}

static uint64_t hash_conn_id(uint64_t x) {
  x ^= x >> 30u;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27u;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31u;
  return x;
}

static void store_u16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void store_u32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void store_u64(uint8_t* p, uint64_t v) {
  for (uint32_t i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (i * 8)) & 0xffu);
}

static uint16_t load_u16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t load_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t load_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < 8; ++i) v |= ((uint64_t)p[i]) << (i * 8);
  return v;
}

static void ring_bind(rudp_ring* r, uint32_t* items, uint32_t cap) {
  r->items = items;
  r->cap = cap;
  r->head = r->tail = r->count = 0;
}

static int ring_push(rudp_ring* r, uint32_t v) {
  if (r->count + 1u >= r->cap) return 0;
  r->items[r->tail] = v;
  r->tail = (r->tail + 1u) % r->cap;
  ++r->count;
  return 1;
}

static int ring_push_front(rudp_ring* r, uint32_t v) {
  if (r->count + 1u >= r->cap) return 0;
  r->head = (r->head + r->cap - 1u) % r->cap;
  r->items[r->head] = v;
  ++r->count;
  return 1;
}

static int ring_peek(const rudp_ring* r, uint32_t* out) {
  if (r->count == 0) return 0;
  *out = r->items[r->head];
  return 1;
}

static int ring_pop(rudp_ring* r, uint32_t* out) {
  if (r->count == 0) return 0;
  *out = r->items[r->head];
  r->head = (r->head + 1u) % r->cap;
  --r->count;
  return 1;
}

static int ring_push_priority(rudp_ring* r, rudp_endpoint* ep, uint32_t v) {
  if (r->count + 1u >= r->cap) return 0;
  uint8_t priority = (v < ep->cfg.max_messages && ep->msgs[v].used)
                         ? ep->msgs[v].priority
                         : 0;
  /* priority 0 can never beat any queued element, so the rotate below would
     always fall through to a tail push; skip the O(n) scan. */
  if (priority == 0) return ring_push(r, v);
  uint32_t n = r->count;
  int inserted = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t old = RUDP_INVALID_INDEX;
    if (!ring_pop(r, &old)) break;
    uint8_t old_priority =
        (old < ep->cfg.max_messages && ep->msgs[old].used)
            ? ep->msgs[old].priority
            : 0;
    if (!inserted && priority > old_priority) {
      (void)ring_push(r, v);
      inserted = 1;
    }
    (void)ring_push(r, old);
  }
  if (!inserted) return ring_push(r, v);
  return 1;
}

static int ring_remove_value(rudp_ring* r, uint32_t v) {
  uint32_t n = r->count;
  int removed = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t old = RUDP_INVALID_INDEX;
    if (!ring_pop(r, &old)) break;
    if (!removed && old == v) {
      removed = 1;
      continue;
    }
    (void)ring_push(r, old);
  }
  return removed;
}

static void init_flow(rudp_flow_state* f, uint16_t flow_id, uint32_t mtu) {
  memset(f, 0, sizeof(*f));
  f->limit.flow_id = flow_id;
  f->limit.max_bps = UINT32_MAX;
  f->limit.max_queue_bytes = UINT32_MAX;
  f->limit.max_delay_ms = 0;
  f->limit.priority = 0;
  f->stats.flow_id = flow_id;
  f->stats.target_bps = UINT32_MAX;
  f->tokens_bytes = UINT32_MAX;
  f->quantum = mtu;
}

static uint64_t flow_token_cap(const rudp_endpoint* ep, const rudp_flow_state* f) {
  if (f->limit.max_bps == UINT32_MAX) return UINT32_MAX;
  if (f->limit.max_bps == 0) return 0;
  uint64_t cap = f->limit.max_bps / 8u;
  uint64_t min_cap = 1500u;
  if (ep && ep->frame_payload > min_cap) min_cap = ep->frame_payload;
  return cap < min_cap ? min_cap : cap;
}

static void write_packet_header(uint8_t* p, uint8_t flags, const rudp_conn* c,
                                uint32_t seq, uint16_t payload_len) {
  store_u32(p + 0, RUDP_PACKET_VERSION_FLAGS(flags));
  store_u64(p + 4, c->conn_id);
  store_u32(p + 12, seq);
  store_u32(p + 16, c->ack.max_seq);
  store_u64(p + 20, c->ack.bits);
  store_u16(p + 28, RUDP_PACKET_HEADER_BYTES);
  store_u16(p + 30, payload_len);
}

static void write_data_frame_header(uint8_t* p, const rudp_msg* m) {
  uint8_t flags = 0;
  if (m->reliability == RUDP_RELIABLE_UNORDERED ||
      m->reliability == RUDP_RELIABLE_ORDERED) {
    flags |= RUDP_F_RELIABLE;
  }
  if (m->reliability == RUDP_RELIABLE_ORDERED) flags |= RUDP_F_ORDERED;
  if (m->reliability == RUDP_UNRELIABLE_SEQUENCED) flags |= RUDP_F_SEQUENCED;
  p[0] = RUDP_FRAME_DATA;
  p[1] = flags;
  store_u16(p + 2, RUDP_DATA_FRAME_HEADER_BYTES);
  store_u16(p + 4, m->flow_id);
  store_u16(p + 6, m->len);
  store_u32(p + 8, m->msg_id);
  store_u16(p + 12, m->frag_index);
  store_u16(p + 14, m->frag_count);
  store_u16(p + 16, m->channel_id);
  store_u16(p + 18, m->channel_seq);
}

static int ack_insert(rudp_ack_tracker* a, uint32_t seq) {
  if (seq == 0) return 0;
  if (a->max_seq == 0) {
    a->max_seq = seq;
    a->bits = 0;
    return 1;
  }
  if (seq32_after(seq, a->max_seq)) {
    uint32_t shift = seq - a->max_seq;
    if (shift > 64u) {
      a->bits = 0;
    } else if (shift == 64u) {
      a->bits = 1ull << 63u;
    } else {
      a->bits = (a->bits << shift) | (1ull << (shift - 1u));
    }
    a->max_seq = seq;
    return 1;
  }
  uint32_t delta = a->max_seq - seq;
  if (delta == 0 || delta > 64u) return 0;
  uint64_t mask = 1ull << (delta - 1u);
  if (a->bits & mask) return 0;
  a->bits |= mask;
  return 1;
}

static rudp_conn* conn_by_index(rudp_endpoint* ep, uint32_t idx) {
  if (idx >= ep->cfg.max_conns) return NULL;
  return ep->conns[idx].active ? &ep->conns[idx] : NULL;
}

static int addr_is_valid(const rudp_addr* a) {
  return a && a->len > 0 && a->len <= sizeof(a->data);
}

static int addr_equal(const rudp_addr* a, const rudp_addr* b) {
  if (!addr_is_valid(a) || !addr_is_valid(b)) return 0;
  return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static rudp_flow_state* flow_for(rudp_conn* c, uint16_t flow_id) {
  if (flow_id >= c->ep->cfg.max_flows) return NULL;
  return &c->flows[flow_id];
}

static rudp_channel_state* channel_for(rudp_conn* c, uint16_t channel_id) {
  if (channel_id >= c->ep->cfg.max_channels) return NULL;
  return &c->channels[channel_id];
}

static rudp_sequenced_state* sequenced_for(rudp_conn* c, uint16_t flow_id,
                                           uint16_t channel_id) {
  if (flow_id >= c->ep->cfg.max_flows ||
      channel_id >= c->ep->cfg.max_channels) {
    return NULL;
  }
  return &c->sequenced[(size_t)flow_id * c->ep->cfg.max_channels + channel_id];
}

static int conn_map_lookup(rudp_endpoint* ep, uint64_t conn_id, uint32_t* out_index) {
  if (!ep || !ep->conn_map || ep->conn_map_cap == 0 || conn_id == 0) return 0;
  uint32_t mask = ep->conn_map_cap - 1u;
  uint32_t pos = (uint32_t)hash_conn_id(conn_id) & mask;
  for (uint32_t probe = 0; probe < ep->conn_map_cap; ++probe) {
    rudp_conn_map_entry* e = &ep->conn_map[(pos + probe) & mask];
    if (!e->used) return 0;
    if (e->conn_id == conn_id) {
      if (out_index) *out_index = e->conn_index;
      return 1;
    }
  }
  return 0;
}

static int conn_map_insert(rudp_endpoint* ep, uint64_t conn_id, uint32_t conn_index) {
  if (!ep || !ep->conn_map || ep->conn_map_cap == 0 || conn_id == 0) return -1;
  uint32_t mask = ep->conn_map_cap - 1u;
  uint32_t pos = (uint32_t)hash_conn_id(conn_id) & mask;
  for (uint32_t probe = 0; probe < ep->conn_map_cap; ++probe) {
    rudp_conn_map_entry* e = &ep->conn_map[(pos + probe) & mask];
    if (!e->used || e->conn_id == conn_id) {
      e->used = 1;
      e->conn_id = conn_id;
      e->conn_index = conn_index;
      return 0;
    }
  }
  return -1;
}

/* linear probing の backward-shift deletion。tombstone を残さないので、
   短命コネクションを繰り返してもテーブルが used エントリで埋まらない。 */
static void conn_map_remove(rudp_endpoint* ep, uint64_t conn_id) {
  if (!ep || !ep->conn_map || ep->conn_map_cap == 0 || conn_id == 0) return;
  uint32_t mask = ep->conn_map_cap - 1u;
  uint32_t pos = (uint32_t)hash_conn_id(conn_id) & mask;
  uint32_t hole = UINT32_MAX;
  for (uint32_t probe = 0; probe < ep->conn_map_cap; ++probe) {
    uint32_t i = (pos + probe) & mask;
    rudp_conn_map_entry* e = &ep->conn_map[i];
    if (!e->used) return;
    if (e->conn_id == conn_id) {
      e->used = 0;
      hole = i;
      break;
    }
  }
  if (hole == UINT32_MAX) return;
  uint32_t i = hole;
  for (;;) {
    i = (i + 1u) & mask;
    rudp_conn_map_entry* e = &ep->conn_map[i];
    if (!e->used) return;
    uint32_t home = (uint32_t)hash_conn_id(e->conn_id) & mask;
    /* e が hole より手前（probe 順で home..i の間に hole が挟まる）なら
       hole へ詰めて新しい hole を e の位置にする。 */
    uint32_t dist_home_to_hole = (hole - home) & mask;
    uint32_t dist_home_to_i = (i - home) & mask;
    if (dist_home_to_hole <= dist_home_to_i) {
      ep->conn_map[hole] = *e;
      e->used = 0;
      hole = i;
    }
  }
}

static uint64_t endpoint_now_ns(rudp_endpoint* ep) {
  if (!ep) return 0;
  if (ep->cfg.socket.now_ns) {
    uint64_t now_ns = ep->cfg.socket.now_ns(ep->cfg.socket.user);
    ep->now_ns = now_ns;
    return now_ns;
  }
  return ep->now_ns;
}

static uint64_t add_ns_saturating(uint64_t now_ns, uint64_t delta_ns) {
  if (UINT64_MAX - now_ns < delta_ns) return UINT64_MAX;
  return now_ns + delta_ns;
}

static uint64_t deadline_after_ms(uint64_t now_ns, uint32_t delay_ms) {
  return add_ns_saturating(now_ns, (uint64_t)delay_ms * RUDP_NS_PER_MS);
}

static void free_msg(rudp_endpoint* ep, uint32_t idx) {
  if (idx >= ep->cfg.max_messages) return;
  rudp_msg* m = &ep->msgs[idx];
  if (!m->used || ep->free_msg_count >= ep->cfg.max_messages) return;
  if (m->lost_ack_seq_count > 0 && m->owner_conn_p1 > 0 &&
      m->owner_conn_p1 <= ep->cfg.max_conns) {
    rudp_conn* owner = &ep->conns[m->owner_conn_p1 - 1u];
    if (owner->late_ack_msg_count > 0) --owner->late_ack_msg_count;
  }
  memset(m, 0, sizeof(*m));
  m->data = ep->msg_data + ((size_t)idx * ep->frame_payload);
  ep->free_msgs[ep->free_msg_count++] = idx;
}

static int msg_is_reliable(const rudp_msg* m) {
  return m->reliability == RUDP_RELIABLE_UNORDERED ||
         m->reliability == RUDP_RELIABLE_ORDERED;
}

static int reliability_is_valid(rudp_reliability reliability) {
  return reliability == RUDP_UNRELIABLE ||
         reliability == RUDP_UNRELIABLE_SEQUENCED ||
         reliability == RUDP_RELIABLE_UNORDERED ||
         reliability == RUDP_RELIABLE_ORDERED;
}

static rudp_ring* queue_for_source(rudp_flow_state* f, uint8_t source) {
  if (source == RUDP_QUEUE_UNRELIABLE) return &f->unreliable_q;
  if (source == RUDP_QUEUE_RELIABLE) return &f->reliable_q;
  if (source == RUDP_QUEUE_RETRY) return &f->retry_q;
  return NULL;
}

static void account_unsent_queue_removed(rudp_conn* c, rudp_flow_state* f,
                                         const rudp_msg* m) {
  if (f && f->queued_bytes >= m->len) {
    f->queued_bytes -= m->len;
    f->stats.queued_bytes = f->queued_bytes;
  }
  if (c && c->send_queue_bytes >= m->len) c->send_queue_bytes -= m->len;
}

static void account_retry_queue_removed(rudp_conn* c, const rudp_msg* m) {
  if (c && c->retransmit_queue_bytes >= m->len) {
    c->retransmit_queue_bytes -= m->len;
  }
}

static void drop_msg(rudp_conn* c, uint32_t mi, uint8_t source, int count_stats) {
  if (!c || mi >= c->ep->cfg.max_messages) return;
  rudp_msg* m = &c->ep->msgs[mi];
  if (!m->used) return;
  rudp_flow_state* f = flow_for(c, m->flow_id);
  if (source == RUDP_QUEUE_UNRELIABLE || source == RUDP_QUEUE_RELIABLE) {
    account_unsent_queue_removed(c, f, m);
  } else if (source == RUDP_QUEUE_RETRY || m->in_retry) {
    account_retry_queue_removed(c, m);
  }
  if (count_stats && f) {
    ++f->stats.send_dropped;
    add_u32_saturating(&f->stats.dropped_bps, m->len);
  }
  free_msg(c->ep, mi);
}

static uint32_t alloc_msg(rudp_endpoint* ep) {
  while (ep->free_msg_count > 0) {
    uint32_t idx = ep->free_msgs[--ep->free_msg_count];
    if (idx >= ep->cfg.max_messages) continue;
    rudp_msg* m = &ep->msgs[idx];
    if (m->used) continue;
    memset(m, 0, sizeof(*m));
    m->used = 1;
    m->data = ep->msg_data + ((size_t)idx * ep->frame_payload);
    return idx;
  }
  return RUDP_INVALID_INDEX;
}

static int enqueue_recv(rudp_conn* c, uint16_t flow_id, uint16_t channel_id,
                        rudp_reliability reliability, const uint8_t* data,
                        uint16_t len, int borrow) {
  rudp_endpoint* ep = c->ep;
  if (ep->recv_count >= ep->cfg.max_recv_events || len > ep->max_payload) return 0;
  rudp_recv_event_i* e = &ep->recv_events[ep->recv_tail];
  e->used = 1;
  (void)borrow;
  e->borrowed = 0;
  e->conn_index = c->index;
  e->conn_id = c->conn_id;
  e->flow_id = flow_id;
  e->channel_id = channel_id;
  e->reliability = reliability;
  e->len = len;
  e->data = ep->recv_event_data + ((size_t)ep->recv_tail * ep->max_payload);
  if (len > 0) memcpy(e->data, data, len);
  ep->recv_tail = (ep->recv_tail + 1u) % ep->cfg.max_recv_events;
  ++ep->recv_count;
  return 1;
}

static void note_packet_rtt(rudp_conn* c, uint64_t rtt_ns) {
  if (rtt_ns == 0) return;
  if (c->min_rtt_ns == 0 || rtt_ns < c->min_rtt_ns) c->min_rtt_ns = rtt_ns;
  if (c->srtt_ns == 0) {
    /* RFC 6298: 初回サンプルで SRTT=R, RTTVAR=R/2 */
    c->srtt_ns = rtt_ns;
    c->rttvar_ns = rtt_ns / 2u;
  } else {
    uint64_t diff = c->srtt_ns > rtt_ns ? c->srtt_ns - rtt_ns
                                        : rtt_ns - c->srtt_ns;
    c->rttvar_ns = (c->rttvar_ns * 3u + diff) / 4u;
    c->srtt_ns = (c->srtt_ns * 7u + rtt_ns) / 8u;
  }
}

#define RUDP_DEFAULT_MIN_RTO_MS 20u
#define RUDP_DEFAULT_MAX_RTO_MS 10000u

/* 適応 RTO（SRTT+4*RTTVAR、クランプ付き）に指数バックオフを乗せた値。
   RTT サンプルが無い間は cfg.rto_ms を初期 RTO として使う。 */
static uint64_t conn_rto_ns(const rudp_conn* c) {
  const rudp_endpoint* ep = c->ep;
  uint64_t rto_ns;
  if (c->srtt_ns == 0) {
    rto_ns = (uint64_t)ep->cfg.rto_ms * RUDP_NS_PER_MS;
  } else {
    uint64_t min_ns = (uint64_t)cfg_u32(ep->cfg.min_rto_ms,
                                        RUDP_DEFAULT_MIN_RTO_MS) *
                      RUDP_NS_PER_MS;
    uint64_t max_ns = (uint64_t)cfg_u32(ep->cfg.max_rto_ms,
                                        RUDP_DEFAULT_MAX_RTO_MS) *
                      RUDP_NS_PER_MS;
    rto_ns = c->srtt_ns + 4u * c->rttvar_ns;
    if (rto_ns < min_ns) rto_ns = min_ns;
    if (rto_ns > max_ns) rto_ns = max_ns;
  }
  uint32_t shift = c->rto_backoff > 6u ? 6u : c->rto_backoff;
  if (rto_ns > (UINT64_MAX >> shift)) return UINT64_MAX;
  return rto_ns << shift;
}

static void release_sent_packet(rudp_conn* c, uint32_t seq, uint64_t now_ns,
                                int complete_refs) {
  rudp_endpoint* ep = c->ep;
  size_t slot = (size_t)c->index * ep->cfg.sent_packet_count +
                (seq % ep->cfg.sent_packet_count);
  rudp_sent_packet* sp = &ep->sent_packets[slot];
  if (!sp->used || sp->conn_index != c->index || sp->seq != seq) return;
  if (complete_refs && now_ns >= sp->sent_ns) note_packet_rtt(c, now_ns - sp->sent_ns);
  if (complete_refs) {
    ++c->acked_packets;
    uint32_t add_bps = (uint32_t)c->ep->mtu * 8u;
    if (c->safe_bps <= UINT32_MAX - add_bps) c->safe_bps += add_bps;
    c->pacing_bps = c->safe_bps;
  }
  for (uint16_t r = 0; r < sp->ref_count; ++r) {
    uint32_t mi = sp->refs[r];
    if (mi >= ep->cfg.max_messages) continue;
    rudp_msg* m = &ep->msgs[mi];
    if (!m->used) continue;
    if (m->inflight_refs > 0) {
      --m->inflight_refs;
      if (c->inflight_bytes >= m->len) c->inflight_bytes -= m->len;
    }
    if (m->carrier_seq == sp->seq) m->carrier_seq = 0;
    if (complete_refs) {
      rudp_flow_state* f = flow_for(c, m->flow_id);
      if (f) add_u32_saturating(&f->stats.delivered_bps, m->len);
      free_msg(ep, mi);
    }
  }
  sp->used = 0;
}

static int ack_covers_seq(uint32_t ack, uint64_t bits, uint32_t seq) {
  if (seq == 0 || ack == 0) return 0;
  if (seq == ack) return 1;
  if (seq32_after(seq, ack)) return 0;
  uint32_t delta = ack - seq;
  if (delta == 0 || delta > 64u) return 0;
  return (bits & (1ull << (delta - 1u))) != 0;
}

static rudp_sent_packet* sent_packet_for_seq(rudp_conn* c, uint32_t seq) {
  rudp_endpoint* ep = c->ep;
  size_t slot = (size_t)c->index * ep->cfg.sent_packet_count +
                (seq % ep->cfg.sent_packet_count);
  rudp_sent_packet* sp = &ep->sent_packets[slot];
  if (!sp->used || sp->conn_index != c->index || sp->seq != seq) return NULL;
  return sp;
}

static void note_msg_lost_seq(rudp_msg* m, uint32_t seq) {
  for (uint8_t i = 0; i < m->lost_ack_seq_count; ++i) {
    if (m->lost_ack_seqs[i] == seq) return;
  }
  if (m->lost_ack_seq_count < RUDP_MAX_LOST_ACK_SEQS) {
    m->lost_ack_seqs[m->lost_ack_seq_count++] = seq;
    return;
  }
  memmove(m->lost_ack_seqs, m->lost_ack_seqs + 1u,
          (RUDP_MAX_LOST_ACK_SEQS - 1u) * sizeof(m->lost_ack_seqs[0]));
  m->lost_ack_seqs[RUDP_MAX_LOST_ACK_SEQS - 1u] = seq;
}

static int msg_lost_seq_is_acked(const rudp_msg* m, uint32_t ack,
                                 uint64_t bits) {
  for (uint8_t i = 0; i < m->lost_ack_seq_count; ++i) {
    if (ack_covers_seq(ack, bits, m->lost_ack_seqs[i])) return 1;
  }
  return 0;
}

static int remove_msg_from_retry_queue(rudp_conn* c, uint32_t mi,
                                       rudp_msg* m) {
  rudp_flow_state* f = flow_for(c, m->flow_id);
  if (!f || !ring_remove_value(&f->retry_q, mi)) return 0;
  m->in_retry = 0;
  if (m->queued_kind == RUDP_QUEUE_RETRY) m->queued_kind = RUDP_QUEUE_NONE;
  account_retry_queue_removed(c, m);
  return 1;
}

static void remove_msg_from_inflight_packets(rudp_conn* c, uint32_t mi,
                                             rudp_msg* m) {
  /* carrier_seq の逆参照で O(1)。以前は sent_packet_count 全走査だった。 */
  if (m->carrier_seq == 0) return;
  rudp_sent_packet* sp = sent_packet_for_seq(c, m->carrier_seq);
  m->carrier_seq = 0;
  if (!sp) return;
  uint16_t write = 0;
  for (uint16_t r = 0; r < sp->ref_count; ++r) {
    if (sp->refs[r] == mi) {
      if (m->inflight_refs > 0) {
        --m->inflight_refs;
        if (c->inflight_bytes >= m->len) c->inflight_bytes -= m->len;
      }
      continue;
    }
    sp->refs[write++] = sp->refs[r];
  }
  sp->ref_count = write;
  if (sp->ref_count == 0) sp->used = 0;
}

static void complete_late_acked_msg(rudp_conn* c, uint32_t mi,
                                    int retry_already_removed) {
  if (!c || mi >= c->ep->cfg.max_messages) return;
  rudp_msg* m = &c->ep->msgs[mi];
  if (!m->used) return;
  if (m->in_retry && !retry_already_removed) {
    (void)remove_msg_from_retry_queue(c, mi, m);
  }
  remove_msg_from_inflight_packets(c, mi, m);
  rudp_flow_state* f = flow_for(c, m->flow_id);
  if (f) add_u32_saturating(&f->stats.delivered_bps, m->len);
  free_msg(c->ep, mi);
}

static void remove_msg_from_current_queue(rudp_conn* c, uint32_t mi,
                                          rudp_msg* m);

static void release_late_acked_messages(rudp_conn* c, uint32_t ack,
                                        uint64_t bits) {
  /* Only messages that lost a packet can be late-acked; without any, both
     rescue scans below are guaranteed no-ops. This runs per received ACK,
     so the fast path matters. */
  if (c->late_ack_msg_count == 0) return;
  rudp_endpoint* ep = c->ep;
  for (uint32_t flow_id = 0; flow_id < ep->cfg.max_flows; ++flow_id) {
    rudp_flow_state* f = &c->flows[flow_id];
    uint32_t n = f->retry_q.count;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t mi = RUDP_INVALID_INDEX;
      if (!ring_pop(&f->retry_q, &mi)) break;
      rudp_msg* m = (mi < ep->cfg.max_messages) ? &ep->msgs[mi] : NULL;
      if (m && m->used && msg_lost_seq_is_acked(m, ack, bits)) {
        m->in_retry = 0;
        if (m->queued_kind == RUDP_QUEUE_RETRY) m->queued_kind = RUDP_QUEUE_NONE;
        account_retry_queue_removed(c, m);
        complete_late_acked_msg(c, mi, 1);
        continue;
      }
      (void)ring_push(&f->retry_q, mi);
    }
  }

  /* retry キュー以外（in-flight や unsent 再投入済み）の late-ack 救済。
     以前は sent_packets×refs の入れ子走査を 1 件完了するたびに先頭から
     再起動していて最悪 O(n^2)。owner でフィルタした msgs の単一走査に置換。 */
  if (c->late_ack_msg_count == 0) return;
  for (uint32_t mi = 0; mi < ep->cfg.max_messages; ++mi) {
    rudp_msg* m = &ep->msgs[mi];
    if (!m->used || m->owner_conn_p1 != c->index + 1u) continue;
    if (m->lost_ack_seq_count == 0) continue;
    if (!msg_lost_seq_is_acked(m, ack, bits)) continue;
    if (m->queued_kind == RUDP_QUEUE_UNRELIABLE ||
        m->queued_kind == RUDP_QUEUE_RELIABLE) {
      /* unsent キューに再投入されていた場合は queued_bytes の帳尻を合わせて
         リングからも抜く。 */
      remove_msg_from_current_queue(c, mi, m);
    }
    complete_late_acked_msg(c, mi, 0);
    if (c->late_ack_msg_count == 0) break;
  }
}

static void remove_msg_from_current_queue(rudp_conn* c, uint32_t mi,
                                          rudp_msg* m) {
  rudp_flow_state* f = flow_for(c, m->flow_id);
  if (!f) return;
  if (m->queued_kind == RUDP_QUEUE_UNRELIABLE) {
    if (ring_remove_value(&f->unreliable_q, mi)) {
      account_unsent_queue_removed(c, f, m);
    }
  } else if (m->queued_kind == RUDP_QUEUE_RELIABLE) {
    if (ring_remove_value(&f->reliable_q, mi)) {
      account_unsent_queue_removed(c, f, m);
    }
  } else if (m->queued_kind == RUDP_QUEUE_RETRY || m->in_retry) {
    (void)remove_msg_from_retry_queue(c, mi, m);
  }
}

static void drop_conn_recv_events(rudp_conn* c) {
  rudp_endpoint* ep = c->ep;
  uint32_t old_count = ep->recv_count;
  uint32_t old_head = ep->recv_head;
  uint32_t write = old_head;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < old_count; ++i) {
    uint32_t read = (old_head + i) % ep->cfg.max_recv_events;
    rudp_recv_event_i* src = &ep->recv_events[read];
    if (!src->used || src->conn_index == c->index) {
      src->used = 0;
      continue;
    }
    if (read != write) {
      rudp_recv_event_i* dst = &ep->recv_events[write];
      *dst = *src;
      dst->data = ep->recv_event_data + ((size_t)write * ep->max_payload);
      if (dst->len > 0) memcpy(dst->data, src->data, dst->len);
      src->used = 0;
    }
    write = (write + 1u) % ep->cfg.max_recv_events;
    ++kept;
  }
  ep->recv_head = old_head;
  ep->recv_tail = write;
  ep->recv_count = kept;
}

static void drop_conn_ordered_holds(rudp_conn* c) {
  rudp_endpoint* ep = c->ep;
  for (uint32_t i = 0; i < ep->cfg.max_ordered_holds; ++i) {
    rudp_ordered_hold* h = &ep->holds[i];
    if (!h->used || h->conn_index != c->index) continue;
    h->used = 0;
    if (ep->ordered_hold_count > 0) --ep->ordered_hold_count;
  }
}

static void drop_conn_reassembly(rudp_conn* c) {
  rudp_endpoint* ep = c->ep;
  for (uint32_t i = 0; i < ep->reasm_count; ++i) {
    rudp_rx_reasm* r = &ep->reasm[i];
    if (r->used && r->conn_index == c->index) r->used = 0;
  }
}

static void abort_conn_internal(rudp_conn* c) {
  if (!c || !c->active) return;
  rudp_endpoint* ep = c->ep;
  for (uint32_t mi = 0; mi < ep->cfg.max_messages; ++mi) {
    rudp_msg* m = &ep->msgs[mi];
    if (!m->used || m->owner_conn_p1 != c->index + 1u) continue;
    remove_msg_from_current_queue(c, mi, m);
    remove_msg_from_inflight_packets(c, mi, m);
    free_msg(ep, mi);
  }
  size_t base = (size_t)c->index * ep->cfg.sent_packet_count;
  for (uint32_t i = 0; i < ep->cfg.sent_packet_count; ++i) {
    rudp_sent_packet* sp = &ep->sent_packets[base + i];
    if (sp->conn_index == c->index) memset(sp, 0, sizeof(*sp));
    ep->sent_seq_queue[base + i] = 0;
  }
  drop_conn_recv_events(c);
  drop_conn_ordered_holds(c);
  drop_conn_reassembly(c);
  c->inflight_bytes = 0;
  c->send_queue_bytes = 0;
  c->retransmit_queue_bytes = 0;
  c->sent_seq_head = 0;
  c->sent_seq_tail = 0;
  c->sent_seq_count = 0;
  c->late_ack_msg_count = 0;
  c->next_retx_ns = 0;
  c->active = 0;
  /* エントリを消さないとスロット再利用時に古い conn_id の lookup が
     別コネクションを返し、tombstone 無しの probing では insert も枯れる。 */
  conn_map_remove(ep, c->conn_id);
  c->conn_id = 0;
}

void rudp_conn_abort(rudp_conn* conn) {
  abort_conn_internal(conn);
}

static void mark_packet_lost(rudp_conn* c, rudp_sent_packet* sp,
                             uint64_t now_ns);

static void prune_sent_seq_queue(rudp_conn* c) {
  rudp_endpoint* ep = c->ep;
  uint32_t cap = ep->cfg.sent_packet_count;
  if (cap == 0 || !ep->sent_seq_queue || c->sent_seq_count == 0) return;
  size_t base = (size_t)c->index * cap;
  uint32_t n = c->sent_seq_count;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t seq = ep->sent_seq_queue[base + c->sent_seq_head];
    c->sent_seq_head = (c->sent_seq_head + 1u) % cap;
    --c->sent_seq_count;
    if (!sent_packet_for_seq(c, seq)) continue;
    ep->sent_seq_queue[base + c->sent_seq_tail] = seq;
    c->sent_seq_tail = (c->sent_seq_tail + 1u) % cap;
    ++c->sent_seq_count;
  }
}

static void track_reliable_seq(rudp_conn* c, uint32_t seq) {
  rudp_endpoint* ep = c->ep;
  uint32_t cap = ep->cfg.sent_packet_count;
  if (cap == 0 || !ep->sent_seq_queue) return;
  size_t base = (size_t)c->index * cap;
  if (c->sent_seq_count >= cap) {
    prune_sent_seq_queue(c);
  }
  if (c->sent_seq_count >= cap) {
    c->sent_seq_head = (c->sent_seq_head + 1u) % cap;
    --c->sent_seq_count;
  }
  ep->sent_seq_queue[base + c->sent_seq_tail] = seq;
  c->sent_seq_tail = (c->sent_seq_tail + 1u) % cap;
  ++c->sent_seq_count;
}

static void release_acked_packets(rudp_conn* c, uint32_t ack, uint64_t bits,
                                  uint64_t now_ns) {
  if (ack == 0) return;
  rudp_endpoint* ep = c->ep;
  uint32_t cap = ep->cfg.sent_packet_count;
  if (cap > 0 && ep->sent_seq_queue && c->sent_seq_count > 0) {
    size_t base = (size_t)c->index * cap;
    uint32_t n = c->sent_seq_count;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t seq = ep->sent_seq_queue[base + c->sent_seq_head];
      c->sent_seq_head = (c->sent_seq_head + 1u) % cap;
      --c->sent_seq_count;
      rudp_sent_packet* sp = sent_packet_for_seq(c, seq);
      if (!sp) continue;
      if (ack_covers_seq(ack, bits, seq)) {
        release_sent_packet(c, seq, now_ns, !sp->lost);
        continue;
      }
      if (sp->lost) {
        release_sent_packet(c, seq, now_ns, 0);
        continue;
      }
      if (seq32_after(ack, seq) && ack - seq >= RUDP_LOSS_PACKET_THRESHOLD) {
        mark_packet_lost(c, sp, now_ns);
        continue;
      }
      ep->sent_seq_queue[base + c->sent_seq_tail] = seq;
      c->sent_seq_tail = (c->sent_seq_tail + 1u) % cap;
      ++c->sent_seq_count;
    }
  }
  release_late_acked_messages(c, ack, bits);
}

static int create_conn_at(rudp_endpoint* ep, uint32_t idx, const rudp_addr* peer,
                          uint64_t conn_id, rudp_conn** out) {
  if (conn_map_insert(ep, conn_id, idx) != 0) return -1;
  rudp_conn* c = &ep->conns[idx];
  memset(c, 0, sizeof(*c));
  c->ep = ep;
  c->index = idx;
  c->active = 1;
  c->peer = *peer;
  c->conn_id = conn_id;
  c->next_packet_seq = 1;
  c->next_msg_id = 1;
  uint64_t now_ns = endpoint_now_ns(ep);
  c->last_rx_ns = now_ns;
  c->last_tx_ns = now_ns;
  c->safe_bps = ep->cfg.initial_safe_bps;
  c->pacing_bps = ep->cfg.initial_safe_bps;
  c->flows = ep->flow_storage + ((size_t)idx * ep->cfg.max_flows);
  c->channels = ep->channel_storage + ((size_t)idx * ep->cfg.max_channels);
  c->sequenced = ep->sequenced_storage +
                 ((size_t)idx * ep->cfg.max_flows * ep->cfg.max_channels);
  for (uint32_t f = 0; f < ep->cfg.max_flows; ++f) {
    init_flow(&c->flows[f], (uint16_t)f, ep->mtu);
    size_t base = ((size_t)idx * ep->cfg.max_flows + f) * 3u *
                  ep->ring_storage_stride;
    ring_bind(&c->flows[f].unreliable_q, ep->ring_storage + base,
              ep->ring_storage_stride);
    ring_bind(&c->flows[f].reliable_q,
              ep->ring_storage + base + ep->ring_storage_stride,
              ep->ring_storage_stride);
    ring_bind(&c->flows[f].retry_q,
              ep->ring_storage + base + ep->ring_storage_stride * 2u,
              ep->ring_storage_stride);
  }
  memset(c->channels, 0, sizeof(rudp_channel_state) * ep->cfg.max_channels);
  memset(c->sequenced, 0,
         sizeof(rudp_sequenced_state) * ep->cfg.max_flows *
             ep->cfg.max_channels);
  if (out) *out = c;
  return 0;
}

int rudp_endpoint_create(rudp_endpoint** out, const rudp_endpoint_config* config) {
  if (!out || !config || !config->socket.send_batch || !config->socket.recv_batch) return -1;
  *out = NULL;
  rudp_endpoint* ep = (rudp_endpoint*)calloc(1, sizeof(*ep));
  if (!ep) return -1;
  ep->cfg = *config;
  ep->cfg.mtu = cfg_u16(ep->cfg.mtu, RUDP_DEFAULT_MTU);
  ep->cfg.max_conns = cfg_u32(ep->cfg.max_conns, RUDP_DEFAULT_MAX_CONNS);
  ep->cfg.max_flows = cfg_u32(ep->cfg.max_flows, RUDP_DEFAULT_MAX_FLOWS);
  ep->cfg.max_channels = cfg_u32(ep->cfg.max_channels, RUDP_DEFAULT_MAX_CHANNELS);
  ep->cfg.max_messages = cfg_u32(ep->cfg.max_messages, RUDP_DEFAULT_MAX_MESSAGES);
  ep->cfg.max_recv_events = cfg_u32(ep->cfg.max_recv_events, RUDP_DEFAULT_MAX_RECV_EVENTS);
  ep->cfg.max_ordered_holds = cfg_u32(ep->cfg.max_ordered_holds, RUDP_DEFAULT_MAX_ORDERED_HOLDS);
  ep->cfg.sent_packet_count = cfg_u32(ep->cfg.sent_packet_count, RUDP_DEFAULT_SENT_PACKETS);
  ep->cfg.recv_batch_size = cfg_u32(ep->cfg.recv_batch_size, RUDP_DEFAULT_RECV_BATCH);
  ep->cfg.send_batch_size = cfg_u32(ep->cfg.send_batch_size, RUDP_DEFAULT_SEND_BATCH);
  ep->cfg.rto_ms = cfg_u32(ep->cfg.rto_ms, RUDP_DEFAULT_RTO_MS);
  ep->cfg.initial_safe_bps = cfg_u32(ep->cfg.initial_safe_bps, UINT32_MAX);
  ep->cfg.max_reassemblies =
      cfg_u32(ep->cfg.max_reassemblies, ep->cfg.max_recv_events);
  if (ep->cfg.max_flows > RUDP_MAX_FLOW_COUNT ||
      ep->cfg.max_channels > RUDP_MAX_CHANNEL_COUNT) {
    goto fail;
  }
  ep->mtu = ep->cfg.mtu;
  if (ep->mtu <= RUDP_PACKET_HEADER_BYTES + RUDP_DATA_FRAME_HEADER_BYTES) goto fail;
  ep->frame_payload = (uint16_t)(ep->mtu - RUDP_PACKET_HEADER_BYTES -
                                 RUDP_DATA_FRAME_HEADER_BYTES);
  ep->max_payload = cfg_u16(ep->cfg.max_payload_bytes, ep->frame_payload);
  if (ep->max_payload == 0) goto fail;
  if ((uint32_t)ep->max_payload >
      (uint32_t)ep->frame_payload * RUDP_MAX_FRAGS_PER_MSG) {
    goto fail;
  }
  ep->per_conn_queue_cap = ep->cfg.max_messages / ep->cfg.max_conns;
  if (ep->per_conn_queue_cap < 1024u) ep->per_conn_queue_cap = 1024u;
  if (ep->per_conn_queue_cap == UINT32_MAX) goto fail;
  ep->ring_storage_stride = ep->per_conn_queue_cap + 1u;
  if (ep->cfg.max_conns > 0x20000000u) goto fail;
  ep->conn_map_cap = next_pow2_u32(ep->cfg.max_conns * 4u);
  if (ep->conn_map_cap == 0) goto fail;
  if (ep->conn_map_cap < 16u) ep->conn_map_cap = 16u;

  size_t flow_count = 0;
  size_t channel_count = 0;
  size_t sequenced_count = 0;
  size_t ring_count = 0;
  size_t sent_packet_total = 0;
  size_t msg_data_bytes = 0;
  size_t hold_data_bytes = 0;
  size_t recv_event_data_bytes = 0;
  size_t reasm_data_bytes = 0;
  size_t recv_batch_data_bytes = 0;
  size_t send_batch_data_bytes = 0;
  size_t send_batch_ref_total = 0;
  if (!checked_mul_size(ep->cfg.max_conns, ep->cfg.max_flows, &flow_count) ||
      !checked_mul_size(ep->cfg.max_conns, ep->cfg.max_channels,
                        &channel_count) ||
      !checked_mul_size(flow_count, ep->cfg.max_channels, &sequenced_count) ||
      !checked_mul_size(flow_count, 3u, &ring_count) ||
      !checked_mul_size(ring_count, ep->ring_storage_stride, &ring_count) ||
      !checked_mul_size(ep->cfg.max_conns, ep->cfg.sent_packet_count,
                        &sent_packet_total) ||
      !checked_mul_size(ep->cfg.max_messages, ep->frame_payload,
                        &msg_data_bytes) ||
      !checked_mul_size(ep->cfg.max_ordered_holds, ep->max_payload,
                        &hold_data_bytes) ||
      !checked_mul_size(ep->cfg.max_recv_events, ep->max_payload,
                        &recv_event_data_bytes) ||
      !checked_mul_size(ep->cfg.max_reassemblies, ep->max_payload,
                        &reasm_data_bytes) ||
      !checked_mul_size(ep->cfg.recv_batch_size, ep->mtu,
                        &recv_batch_data_bytes) ||
      !checked_mul_size(ep->cfg.send_batch_size, ep->mtu,
                        &send_batch_data_bytes) ||
      !checked_mul_size(ep->cfg.send_batch_size, RUDP_MAX_REFS_PER_PACKET,
                        &send_batch_ref_total)) {
    goto fail;
  }

  ep->conns = (rudp_conn*)calloc(ep->cfg.max_conns, sizeof(rudp_conn));
  ep->flow_storage = (rudp_flow_state*)calloc(flow_count,
                                              sizeof(rudp_flow_state));
  ep->channel_storage = (rudp_channel_state*)calloc(channel_count,
                                                    sizeof(rudp_channel_state));
  ep->sequenced_storage = (rudp_sequenced_state*)calloc(
      sequenced_count, sizeof(rudp_sequenced_state));
  ep->ring_storage = (uint32_t*)calloc(ring_count, sizeof(uint32_t));
  ep->conn_map = (rudp_conn_map_entry*)calloc(ep->conn_map_cap,
                                              sizeof(rudp_conn_map_entry));
  ep->msgs = (rudp_msg*)calloc(ep->cfg.max_messages, sizeof(rudp_msg));
  ep->msg_data = (uint8_t*)calloc(msg_data_bytes, 1);
  ep->free_msgs = (uint32_t*)calloc(ep->cfg.max_messages, sizeof(uint32_t));
  ep->sent_packets = (rudp_sent_packet*)calloc(sent_packet_total,
                                               sizeof(rudp_sent_packet));
  ep->sent_seq_queue = (uint32_t*)calloc(sent_packet_total, sizeof(uint32_t));
  ep->holds = (rudp_ordered_hold*)calloc(ep->cfg.max_ordered_holds, sizeof(rudp_ordered_hold));
  ep->hold_data = (uint8_t*)calloc(hold_data_bytes, 1);
  ep->recv_events = (rudp_recv_event_i*)calloc(ep->cfg.max_recv_events, sizeof(rudp_recv_event_i));
  ep->recv_event_data = (uint8_t*)calloc(recv_event_data_bytes, 1);
  ep->borrow_data = (uint8_t*)calloc(1, ep->max_payload);
  ep->reasm_count = ep->cfg.max_reassemblies;
  ep->reasm = (rudp_rx_reasm*)calloc(ep->reasm_count, sizeof(rudp_rx_reasm));
  ep->reasm_data = (uint8_t*)calloc(reasm_data_bytes, 1);
  ep->recv_batch = (rudp_in_packet*)calloc(ep->cfg.recv_batch_size, sizeof(rudp_in_packet));
  ep->recv_batch_data = (uint8_t*)calloc(recv_batch_data_bytes, 1);
  ep->send_batch = (rudp_out_packet*)calloc(ep->cfg.send_batch_size, sizeof(rudp_out_packet));
  ep->send_batch_data = (uint8_t*)calloc(send_batch_data_bytes, 1);
  ep->send_batch_conn = (uint32_t*)calloc(ep->cfg.send_batch_size, sizeof(uint32_t));
  ep->send_batch_refs = (uint32_t*)calloc(send_batch_ref_total,
                                          sizeof(uint32_t));
  ep->send_batch_msgs = (uint32_t*)calloc(send_batch_ref_total,
                                          sizeof(uint32_t));
  ep->send_batch_msg_sources = (uint8_t*)calloc(send_batch_ref_total,
                                                sizeof(uint8_t));
  ep->send_batch_ref_counts = (uint16_t*)calloc(ep->cfg.send_batch_size, sizeof(uint16_t));
  ep->send_batch_msg_counts = (uint16_t*)calloc(ep->cfg.send_batch_size, sizeof(uint16_t));
  ep->send_batch_ack_only = (uint8_t*)calloc(ep->cfg.send_batch_size, sizeof(uint8_t));
  if (!ep->conns || !ep->flow_storage || !ep->channel_storage ||
      !ep->sequenced_storage ||
      !ep->ring_storage || !ep->conn_map || !ep->msgs || !ep->msg_data ||
      !ep->free_msgs || !ep->sent_packets || !ep->sent_seq_queue ||
      !ep->holds || !ep->hold_data ||
      !ep->recv_events || !ep->recv_event_data || !ep->borrow_data ||
      !ep->reasm || !ep->reasm_data ||
      !ep->recv_batch || !ep->recv_batch_data || !ep->send_batch ||
      !ep->send_batch_data || !ep->send_batch_conn || !ep->send_batch_refs ||
      !ep->send_batch_msgs || !ep->send_batch_msg_sources ||
      !ep->send_batch_ref_counts ||
      !ep->send_batch_msg_counts || !ep->send_batch_ack_only) {
    goto fail;
  }
  for (uint32_t i = 0; i < ep->cfg.max_messages; ++i) {
    ep->msgs[i].data = ep->msg_data + ((size_t)i * ep->frame_payload);
    ep->free_msgs[ep->free_msg_count++] = i;
  }
  for (uint32_t i = 0; i < ep->cfg.recv_batch_size; ++i) {
    ep->recv_batch[i].data = ep->recv_batch_data + ((size_t)i * ep->mtu);
    ep->recv_batch[i].cap = ep->mtu;
  }
  *out = ep;
  return 0;

fail:
  rudp_endpoint_destroy(ep);
  return -1;
}

void rudp_endpoint_destroy(rudp_endpoint* ep) {
  if (!ep) return;
  free(ep->conns);
  free(ep->flow_storage);
  free(ep->channel_storage);
  free(ep->sequenced_storage);
  free(ep->ring_storage);
  free(ep->conn_map);
  free(ep->msgs);
  free(ep->msg_data);
  free(ep->free_msgs);
  free(ep->sent_packets);
  free(ep->sent_seq_queue);
  free(ep->holds);
  free(ep->hold_data);
  free(ep->recv_events);
  free(ep->recv_event_data);
  free(ep->borrow_data);
  free(ep->reasm);
  free(ep->reasm_data);
  free(ep->recv_batch);
  free(ep->recv_batch_data);
  free(ep->send_batch);
  free(ep->send_batch_data);
  free(ep->send_batch_conn);
  free(ep->send_batch_refs);
  free(ep->send_batch_msgs);
  free(ep->send_batch_msg_sources);
  free(ep->send_batch_ref_counts);
  free(ep->send_batch_msg_counts);
  free(ep->send_batch_ack_only);
  free(ep);
}

int rudp_endpoint_connect(rudp_endpoint* ep, const rudp_addr* peer, uint64_t conn_id,
                          rudp_conn** out) {
  if (out) *out = NULL;
  if (!ep || !addr_is_valid(peer) || conn_id == 0) return -1;
  if (rudp_endpoint_find_conn(ep, conn_id)) return -1;
  for (uint32_t i = 0; i < ep->cfg.max_conns; ++i) {
    if (!ep->conns[i].active) return create_conn_at(ep, i, peer, conn_id, out);
  }
  return -1;
}

rudp_conn* rudp_endpoint_find_conn(rudp_endpoint* ep, uint64_t conn_id) {
  if (!ep || conn_id == 0) return NULL;
  uint32_t idx = 0;
  if (!conn_map_lookup(ep, conn_id, &idx)) return NULL;
  rudp_conn* c = conn_by_index(ep, idx);
  /* conn_map_remove があるので通常ここは一致するが、スロット再利用で別
     コネクションを返す事故を防ぐため conn_id を必ず照合する（防御）。 */
  if (!c || c->conn_id != conn_id) return NULL;
  return c;
}

uint64_t rudp_conn_id(const rudp_conn* conn) {
  return conn ? conn->conn_id : 0;
}

void rudp_get_status(rudp_conn* conn, rudp_status* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!conn || !conn->active) return;
  out->safe_bps = conn->safe_bps;
  out->pacing_bps = conn->pacing_bps;
  out->rtt_ms = conn->srtt_ns ? (uint32_t)(conn->srtt_ns / RUDP_NS_PER_MS) : 0;
  out->min_rtt_ms = conn->min_rtt_ns ? (uint32_t)(conn->min_rtt_ns / RUDP_NS_PER_MS) : 0;
  out->queue_delay_ms = conn->queue_delay_ms;
  uint64_t total_loss_samples = conn->acked_packets + conn->lost_packets;
  if (total_loss_samples > 0) {
    out->loss_ppm = (uint32_t)((conn->lost_packets * 1000000ull) /
                               total_loss_samples);
  }
  if (conn->recv_packets > 0) {
    out->reorder_ppm = (uint32_t)((conn->reordered_packets * 1000000ull) /
                                  conn->recv_packets);
  }
  out->inflight_bytes = conn->inflight_bytes;
  out->send_queue_bytes = conn->send_queue_bytes;
  out->retransmit_queue_bytes = conn->retransmit_queue_bytes;
  out->mtu = conn->ep->mtu;
  out->usable = 1;
}

void rudp_set_flow_limits(rudp_conn* conn, const rudp_flow_limit* limits, size_t count) {
  if (!conn || !limits) return;
  for (size_t i = 0; i < count; ++i) {
    if (limits[i].flow_id >= conn->ep->cfg.max_flows) continue;
    rudp_flow_state* f = &conn->flows[limits[i].flow_id];
    f->limit = limits[i];
    f->stats.target_bps = limits[i].max_bps;
    f->quantum = conn->ep->mtu * (uint32_t)(1u + limits[i].priority);
    if (f->limit.max_bps == UINT32_MAX) {
      f->tokens_bytes = UINT32_MAX;
    } else {
      uint64_t cap = flow_token_cap(conn->ep, f);
      if (f->tokens_bytes > cap) f->tokens_bytes = cap;
    }
  }
}

static void refill_flow(rudp_endpoint* ep, rudp_flow_state* f, uint64_t now_ns) {
  if (f->limit.max_bps == UINT32_MAX) {
    f->tokens_bytes = UINT32_MAX;
    f->last_refill_ns = now_ns;
    return;
  }
  if (f->last_refill_ns == 0) {
    f->last_refill_ns = now_ns;
    f->tokens_bytes = flow_token_cap(ep, f);
    return;
  }
  if (now_ns <= f->last_refill_ns) return;
  uint64_t cap = flow_token_cap(ep, f);
  if (f->tokens_bytes < cap && f->limit.max_bps > 0) {
    uint64_t elapsed = now_ns - f->last_refill_ns;
    uint64_t need = cap - f->tokens_bytes;
    uint64_t denom = 8ull * RUDP_NS_PER_SEC;
    uint64_t need_ns = need > UINT64_MAX / denom ? UINT64_MAX : need * denom;
    uint64_t full_after = need_ns / f->limit.max_bps +
                          ((need_ns % f->limit.max_bps) != 0 ? 1u : 0u);
    if (elapsed >= full_after) {
      f->tokens_bytes = cap;
    } else {
      uint64_t add = ((uint64_t)f->limit.max_bps * elapsed) / denom;
      f->tokens_bytes += add;
    }
  }
  f->last_refill_ns = now_ns;
}

static int opts_replace_unreliable(const rudp_send_opts* opts) {
  if (!opts->replace_key) return 0;
  if (opts->reliability == RUDP_RELIABLE_UNORDERED ||
      opts->reliability == RUDP_RELIABLE_ORDERED) {
    return 0;
  }
  return 1;
}

static int msg_matches_replacement(const rudp_msg* m,
                                   const rudp_send_opts* opts) {
  return m && m->used && m->flow_id == opts->flow_id &&
         m->channel_id == opts->channel_id &&
         m->reliability == opts->reliability &&
         m->replace_key == opts->replace_key;
}

static void measure_replaced_unreliable(rudp_conn* conn,
                                        const rudp_flow_state* f,
                                        const rudp_send_opts* opts,
                                        uint32_t* out_count,
                                        uint32_t* out_bytes) {
  uint32_t count = 0;
  uint32_t bytes = 0;
  if (opts_replace_unreliable(opts) && f->unreliable_q.cap > 0) {
    const rudp_ring* q = &f->unreliable_q;
    for (uint32_t i = 0; i < q->count; ++i) {
      uint32_t pos = (q->head + i) % q->cap;
      uint32_t mi = q->items[pos];
      const rudp_msg* m =
          (mi < conn->ep->cfg.max_messages) ? &conn->ep->msgs[mi] : NULL;
      if (msg_matches_replacement(m, opts)) {
        ++count;
        add_u32_saturating(&bytes, m->len);
      }
    }
  }
  if (out_count) *out_count = count;
  if (out_bytes) *out_bytes = bytes;
}

static void drop_replaced_unreliable(rudp_conn* conn, rudp_flow_state* f,
                                     const rudp_send_opts* opts) {
  if (!opts_replace_unreliable(opts)) return;
  uint32_t n = f->unreliable_q.count;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t mi = RUDP_INVALID_INDEX;
    if (!ring_pop(&f->unreliable_q, &mi)) break;
    rudp_msg* m = (mi < conn->ep->cfg.max_messages) ? &conn->ep->msgs[mi] : NULL;
    if (msg_matches_replacement(m, opts)) {
      drop_msg(conn, mi, RUDP_QUEUE_UNRELIABLE, 1);
      continue;
    }
    (void)ring_push(&f->unreliable_q, mi);
  }
}

rudp_send_result rudp_send(rudp_conn* conn, const void* data, size_t len,
                           const rudp_send_opts* opts) {
  if (!conn || !conn->active || (len > 0 && !data)) return RUDP_SEND_UNUSABLE;
  rudp_send_opts def;
  memset(&def, 0, sizeof(def));
  def.reliability = RUDP_UNRELIABLE;
  if (!opts) opts = &def;
  if (!reliability_is_valid(opts->reliability)) return RUDP_SEND_UNUSABLE;
  if (opts->flow_id >= conn->ep->cfg.max_flows ||
      opts->channel_id >= conn->ep->cfg.max_channels ||
      len > conn->ep->max_payload) {
    return RUDP_SEND_QUEUE_FULL;
  }
  rudp_flow_state* f = &conn->flows[opts->flow_id];
  if (f->limit.max_bps == 0) {
    ++f->stats.send_rate_limited;
    return RUDP_SEND_RATE_LIMITED;
  }
  uint16_t frag_count = (uint16_t)((len + conn->ep->frame_payload - 1u) /
                                   conn->ep->frame_payload);
  if (frag_count == 0) frag_count = 1;
  if (frag_count > RUDP_MAX_FRAGS_PER_MSG) {
    ++f->stats.send_queue_full;
    return RUDP_SEND_QUEUE_FULL;
  }
  rudp_channel_state* ch = channel_for(conn, opts->channel_id);
  if (!ch) return RUDP_SEND_QUEUE_FULL;
  rudp_ring* q = (opts->reliability == RUDP_RELIABLE_UNORDERED ||
                  opts->reliability == RUDP_RELIABLE_ORDERED)
                     ? &f->reliable_q
                     : &f->unreliable_q;
  uint8_t queued_kind = (q == &f->reliable_q) ? RUDP_QUEUE_RELIABLE
                                              : RUDP_QUEUE_UNRELIABLE;
  uint32_t max_q_items = q->cap > 0 ? q->cap - 1u : 0;
  if ((f->limit.max_queue_bytes != UINT32_MAX &&
       len > f->limit.max_queue_bytes) ||
      frag_count > conn->ep->cfg.max_messages ||
      frag_count > max_q_items) {
    ++f->stats.send_queue_full;
    return RUDP_SEND_QUEUE_FULL;
  }
  uint32_t replace_count = 0;
  uint32_t replace_bytes = 0;
  measure_replaced_unreliable(conn, f, opts, &replace_count, &replace_bytes);
  if (f->limit.max_queue_bytes != UINT32_MAX) {
    uint32_t queued_after_replace =
        f->queued_bytes > replace_bytes ? f->queued_bytes - replace_bytes : 0;
    if (queued_after_replace > f->limit.max_queue_bytes ||
        len > (size_t)(f->limit.max_queue_bytes - queued_after_replace)) {
      ++f->stats.send_queue_full;
      return RUDP_SEND_QUEUE_FULL;
    }
  }
  if ((uint64_t)conn->ep->free_msg_count + replace_count < frag_count) {
    ++f->stats.send_queue_full;
    return RUDP_SEND_QUEUE_FULL;
  }
  uint32_t available = (q->cap > q->count) ? q->cap - q->count - 1u : 0;
  if ((uint64_t)available + replace_count < frag_count) {
    ++f->stats.send_queue_full;
    return RUDP_SEND_QUEUE_FULL;
  }
  drop_replaced_unreliable(conn, f, opts);
  uint32_t allocated[RUDP_MAX_FRAGS_PER_MSG];
  uint32_t msg_id = conn->next_msg_id++;
  /* channel_seq はここでは「予約」のみ。ordered の seq を先にインクリメント
     してから投入に失敗すると seq に穴が空き、受信側 expected_ordered が
     永久に待つ。採番の確定（インクリメント）は全フラグメントのキュー投入が
     成功した後に行う。 */
  uint16_t channel_seq = 0;
  rudp_sequenced_state* seq_state = NULL;
  if (opts->reliability == RUDP_RELIABLE_ORDERED) {
    channel_seq = ch->next_ordered_seq;
  } else if (opts->reliability == RUDP_UNRELIABLE_SEQUENCED) {
    seq_state = sequenced_for(conn, opts->flow_id, opts->channel_id);
    if (!seq_state) {
      ++f->stats.send_queue_full;
      return RUDP_SEND_QUEUE_FULL;
    }
    channel_seq = seq_state->next_send_seq;
  }
  uint64_t enqueue_ns = endpoint_now_ns(conn->ep);
  uint64_t deadline_ns =
      opts->deadline_ms ? deadline_after_ms(enqueue_ns, opts->deadline_ms) : 0;
  if (f->limit.max_delay_ms > 0) {
    uint64_t flow_deadline = deadline_after_ms(enqueue_ns, f->limit.max_delay_ms);
    if (deadline_ns == 0 || flow_deadline < deadline_ns) deadline_ns = flow_deadline;
  }
  for (uint16_t frag = 0; frag < frag_count; ++frag) {
    uint32_t mi = alloc_msg(conn->ep);
    if (mi == RUDP_INVALID_INDEX) {
      for (uint16_t i = 0; i < frag; ++i) free_msg(conn->ep, allocated[i]);
      ++f->stats.send_queue_full;
      return RUDP_SEND_QUEUE_FULL;
    }
    allocated[frag] = mi;
    rudp_msg* m = &conn->ep->msgs[mi];
    m->owner_conn_p1 = conn->index + 1u;
    m->reliability = opts->reliability;
    m->flow_id = opts->flow_id;
    m->channel_id = opts->channel_id;
    m->channel_seq = channel_seq;
    m->frag_index = frag;
    m->frag_count = frag_count;
    m->msg_id = msg_id;
    m->replace_key = opts->replace_key;
    m->enqueue_ns = enqueue_ns;
    m->deadline_ns = deadline_ns;
    m->queued_kind = queued_kind;
    m->priority = opts->priority;
    size_t offset = (size_t)frag * conn->ep->frame_payload;
    size_t remaining = (len > offset) ? len - offset : 0;
    size_t frag_len = remaining > conn->ep->frame_payload
                          ? conn->ep->frame_payload
                          : remaining;
    m->len = (uint16_t)frag_len;
    if (frag_len > 0) memcpy(m->data, (const uint8_t*)data + offset, frag_len);
  }
  for (uint16_t frag = 0; frag < frag_count; ++frag) {
    if (!ring_push_priority(q, conn->ep, allocated[frag])) {
      /* 事前の空き検査で通常は起きないが、万一失敗したら投入済み分を
         取り除いて全フラグメントを解放し、seq を消費せずに失敗を返す。 */
      for (uint16_t undo = 0; undo < frag; ++undo) {
        (void)ring_remove_value(q, allocated[undo]);
      }
      for (uint16_t i = 0; i < frag_count; ++i) free_msg(conn->ep, allocated[i]);
      ++f->stats.send_queue_full;
      return RUDP_SEND_QUEUE_FULL;
    }
  }
  /* 全フラグメントの投入が成功したのでここで初めて採番を確定させる。 */
  if (opts->reliability == RUDP_RELIABLE_ORDERED) {
    ch->next_ordered_seq = (uint16_t)(channel_seq + 1u);
  } else if (seq_state) {
    seq_state->next_send_seq = (uint16_t)(channel_seq + 1u);
  }
  add_u32_saturating(&f->queued_bytes, (uint32_t)len);
  f->stats.queued_bytes = f->queued_bytes;
  ++f->stats.send_ok;
  add_u32_saturating(&conn->send_queue_bytes, (uint32_t)len);
  return RUDP_SEND_QUEUED;
}

size_t rudp_get_flow_stats(rudp_conn* conn, rudp_flow_stats* out, size_t max_count) {
  if (!conn || !out || max_count == 0) return 0;
  size_t n = 0;
  for (uint32_t i = 0; i < conn->ep->cfg.max_flows && n < max_count; ++i) {
    out[n++] = conn->flows[i].stats;
  }
  return n;
}

static int pick_from_queue(rudp_conn* c, rudp_flow_state* f, rudp_ring* q,
                           uint64_t now_ns, uint8_t source, int use_deficit,
                           uint32_t* out, uint8_t* out_source) {
  uint32_t mi;
  while (ring_peek(q, &mi)) {
    if (mi >= c->ep->cfg.max_messages) {
      (void)ring_pop(q, &mi);
      continue;
    }
    rudp_msg* m = &c->ep->msgs[mi];
    if (!m->used) {
      (void)ring_pop(q, &mi);
      continue;
    }
    /* Without an explicit skip frame, dropping ordered reliable data creates
       a permanent receiver-side sequence gap. */
    if (m->deadline_ns && now_ns >= m->deadline_ns &&
        m->reliability != RUDP_RELIABLE_ORDERED) {
      (void)ring_pop(q, &mi);
      ++f->stats.expired;
      drop_msg(c, mi, source, 1);
      continue;
    }
    uint32_t cost = (uint32_t)m->len + RUDP_DATA_FRAME_HEADER_BYTES;
    if (use_deficit && f->deficit < cost) return 0;
    if (f->limit.max_bps != UINT32_MAX && f->tokens_bytes < m->len) return 0;
    (void)ring_pop(q, &mi);
    if (source == RUDP_QUEUE_RETRY) {
      m->in_retry = 0;
      account_retry_queue_removed(c, m);
    }
    if (f->limit.max_bps != UINT32_MAX) f->tokens_bytes -= m->len;
    if (use_deficit) f->deficit -= cost;
    uint64_t delay_ns = (now_ns > m->enqueue_ns) ? now_ns - m->enqueue_ns : 0;
    uint32_t delay_ms = (uint32_t)(delay_ns / RUDP_NS_PER_MS);
    f->stats.queue_delay_ms = delay_ms;
    c->queue_delay_ms = delay_ms;
    *out = mi;
    if (out_source) *out_source = source;
    return 1;
  }
  return 0;
}

static int pick_from_flow(rudp_conn* c, rudp_flow_state* f, uint64_t now_ns,
                          uint32_t* out, uint8_t* out_source) {
  if (f->retry_q.count > 0) {
    if (pick_from_queue(c, f, &f->retry_q, now_ns, RUDP_QUEUE_RETRY, 0,
                        out, out_source)) {
      return 1;
    }
    if (f->retry_q.count > 0) return 0;
  }
  if (pick_from_queue(c, f, &f->reliable_q, now_ns, RUDP_QUEUE_RELIABLE, 1, out,
                      out_source)) {
    return 1;
  }
  return pick_from_queue(c, f, &f->unreliable_q, now_ns, RUDP_QUEUE_UNRELIABLE,
                         1, out, out_source);
}

static int pick_next_msg(rudp_conn* c, uint64_t now_ns, uint32_t* out,
                         uint8_t* out_source) {
  for (uint32_t i = 0; i < c->ep->cfg.max_flows; ++i) {
    rudp_flow_state* f = &c->flows[i];
    if (f->retry_q.count > 0 &&
        pick_from_flow(c, f, now_ns, out, out_source)) {
      return 1;
    }
  }
  for (uint32_t tries = 0; tries < c->ep->cfg.max_flows; ++tries) {
    uint32_t idx = (c->flow_cursor + tries) % c->ep->cfg.max_flows;
    rudp_flow_state* f = &c->flows[idx];
    refill_flow(c->ep, f, now_ns);
    f->deficit = (f->deficit > UINT32_MAX - f->quantum)
                     ? UINT32_MAX
                     : f->deficit + f->quantum;
    if (pick_from_flow(c, f, now_ns, out, out_source)) {
      c->flow_cursor = (idx + 1u) % c->ep->cfg.max_flows;
      return 1;
    }
  }
  return 0;
}

static int queue_retransmit(rudp_conn* c, uint32_t mi) {
  rudp_msg* m = &c->ep->msgs[mi];
  if (!m->used) return 0;
  if (m->in_retry) return 1;
  rudp_flow_state* f = flow_for(c, m->flow_id);
  if (!f) return 0;
  if (ring_push_priority(&f->retry_q, c->ep, mi)) {
    m->in_retry = 1;
    m->queued_kind = RUDP_QUEUE_RETRY;
    add_u32_saturating(&c->retransmit_queue_bytes, m->len);
    ++f->stats.retransmits;
    return 1;
  }
  return 0;
}

static void mark_packet_lost(rudp_conn* c, rudp_sent_packet* sp,
                             uint64_t now_ns) {
  if (!c || !sp || sp->lost) return;
  sp->lost = 1;
  ++c->lost_packets;
  /* loss 応答は 1 RTT（サンプル前は最小 RTO）につき 1 回。ウィンドウ分の
     一括ロスでパケット毎に半減させるとレートが一気に床まで落ちる。 */
  uint64_t halve_interval_ns =
      c->srtt_ns ? c->srtt_ns
                 : (uint64_t)RUDP_DEFAULT_MIN_RTO_MS * RUDP_NS_PER_MS;
  if (c->last_rate_halve_ns == 0 ||
      now_ns >= add_ns_saturating(c->last_rate_halve_ns, halve_interval_ns)) {
    if (c->safe_bps > 64000u) c->safe_bps /= 2u;
    c->pacing_bps = c->safe_bps;
    c->last_rate_halve_ns = now_ns;
  }
  for (uint16_t r = 0; r < sp->ref_count; ++r) {
    uint32_t mi = sp->refs[r];
    if (mi >= c->ep->cfg.max_messages) continue;
    rudp_msg* m = &c->ep->msgs[mi];
    if (!m->used) continue;
    if (m->lost_ack_seq_count == 0) ++c->late_ack_msg_count;
    note_msg_lost_seq(m, sp->seq);
    if (m->inflight_refs > 0) {
      --m->inflight_refs;
      if (c->inflight_bytes >= m->len) c->inflight_bytes -= m->len;
    }
    if (m->carrier_seq == sp->seq) m->carrier_seq = 0;
    if (!queue_retransmit(c, mi)) {
      drop_msg(c, mi, RUDP_QUEUE_NONE, 1);
    }
    if (!c->active) return;
  }
  sp->used = 0;
}

static void service_retransmits(rudp_conn* c, uint64_t now_ns) {
  if (c->next_retx_ns == 0 || now_ns < c->next_retx_ns) return;
  rudp_endpoint* ep = c->ep;
  uint64_t rto_ns = conn_rto_ns(c);
  uint64_t next_retx_ns = 0;
  int lost_any = 0;
  /* sent_seq_queue（送信時刻順）を 1 周して RTO 判定する。スロット配列の
     全走査（sent_packet_count 固定）ではなく生きている追跡 seq 数に比例。 */
  uint32_t cap = ep->cfg.sent_packet_count;
  if (cap > 0 && ep->sent_seq_queue && c->sent_seq_count > 0) {
    size_t base = (size_t)c->index * cap;
    uint32_t n = c->sent_seq_count;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t seq = ep->sent_seq_queue[base + c->sent_seq_head];
      c->sent_seq_head = (c->sent_seq_head + 1u) % cap;
      --c->sent_seq_count;
      rudp_sent_packet* sp = sent_packet_for_seq(c, seq);
      if (!sp || sp->lost) continue;
      uint64_t due = add_ns_saturating(sp->sent_ns, rto_ns);
      if (now_ns >= due) {
        lost_any = 1;
        mark_packet_lost(c, sp, now_ns);
        if (!c->active) return;
        continue;
      }
      if (next_retx_ns == 0 || due < next_retx_ns) next_retx_ns = due;
      ep->sent_seq_queue[base + c->sent_seq_tail] = seq;
      c->sent_seq_tail = (c->sent_seq_tail + 1u) % cap;
      ++c->sent_seq_count;
    }
  }
  c->next_retx_ns = next_retx_ns;
  if (lost_any) {
    /* 死活判定はパケット数ではなく「受信の無いまま経過した RTO ラウンド数」。
       ラウンド毎に RTO を指数バックオフさせ再送洪水も抑える。
       max_retransmits は「許容する再送ラウンド数」なので超過（>）で abort。 */
    ++c->retx_rounds_since_rx;
    if (c->rto_backoff < 6u) ++c->rto_backoff;
    if (ep->cfg.max_retransmits > 0 &&
        c->retx_rounds_since_rx > ep->cfg.max_retransmits) {
      abort_conn_internal(c);
    }
  }
}

static void record_sent_packet(rudp_endpoint* ep, uint32_t batch_index, uint64_t now_ns) {
  uint16_t refs = ep->send_batch_ref_counts[batch_index];
  if (refs == 0) return;
  rudp_conn* c = conn_by_index(ep, ep->send_batch_conn[batch_index]);
  if (!c) return;
  c->last_tx_ns = now_ns;
  const uint8_t* data = ep->send_batch[batch_index].data;
  uint32_t seq = load_u32(data + 12);
  size_t slot = (size_t)c->index * ep->cfg.sent_packet_count +
                (seq % ep->cfg.sent_packet_count);
  rudp_sent_packet* sp = &ep->sent_packets[slot];
  memset(sp, 0, sizeof(*sp));
  sp->used = 1;
  sp->conn_index = c->index;
  sp->seq = seq;
  sp->sent_ns = now_ns;
  sp->bytes = (uint16_t)ep->send_batch[batch_index].len;
  sp->ref_count = refs;
  track_reliable_seq(c, seq);
  uint64_t due = add_ns_saturating(now_ns, conn_rto_ns(c));
  if (c->next_retx_ns == 0 || due < c->next_retx_ns) c->next_retx_ns = due;
  for (uint16_t i = 0; i < refs; ++i) {
    uint32_t mi = ep->send_batch_refs[(size_t)batch_index * RUDP_MAX_REFS_PER_PACKET + i];
    sp->refs[i] = mi;
    if (mi >= ep->cfg.max_messages) continue;
    rudp_msg* m = &ep->msgs[mi];
    if (!m->used) continue;
    ++m->inflight_refs;
    m->carrier_seq = seq;
    uint8_t source = m->queued_kind;
    m->queued_kind = RUDP_QUEUE_NONE;
    add_u32_saturating(&c->inflight_bytes, m->len);
    rudp_flow_state* f = flow_for(c, m->flow_id);
    if (source == RUDP_QUEUE_UNRELIABLE || source == RUDP_QUEUE_RELIABLE) {
      account_unsent_queue_removed(c, f, m);
    }
  }
}

static void requeue_unsent_msg(rudp_conn* c, uint32_t mi, uint8_t source) {
  if (!c || mi >= c->ep->cfg.max_messages) return;
  rudp_msg* m = &c->ep->msgs[mi];
  if (!m->used) return;
  rudp_flow_state* f = flow_for(c, m->flow_id);
  rudp_ring* q = f ? queue_for_source(f, source) : NULL;
  if (!q) {
    drop_msg(c, mi, source, 1);
    return;
  }
  if (!ring_push_front(q, mi)) {
    drop_msg(c, mi, source, 1);
    return;
  }
  m->queued_kind = source;
  if (source == RUDP_QUEUE_RETRY) {
    m->in_retry = 1;
    add_u32_saturating(&c->retransmit_queue_bytes, m->len);
  }
}

static void requeue_unsent_packet(rudp_endpoint* ep, uint32_t batch_index) {
  rudp_conn* c = conn_by_index(ep, ep->send_batch_conn[batch_index]);
  if (!c || ep->send_batch_ack_only[batch_index]) return;
  uint16_t msg_count = ep->send_batch_msg_counts[batch_index];
  for (uint16_t j = msg_count; j > 0; --j) {
    size_t slot = (size_t)batch_index * RUDP_MAX_REFS_PER_PACKET + (j - 1u);
    uint32_t mi = ep->send_batch_msgs[slot];
    uint8_t source = ep->send_batch_msg_sources[slot];
    requeue_unsent_msg(c, mi, source);
  }
}

static int flush_send_batch(rudp_endpoint* ep, uint64_t now_ns) {
  if (ep->send_count == 0) return 1;
  uint32_t old_count = ep->send_count;
  int sent = ep->cfg.socket.send_batch(ep->cfg.socket.user, ep->send_batch, old_count);
  if (sent < 0) sent = 0;
  if ((uint32_t)sent > old_count) sent = (int)old_count;
  for (uint32_t i = 0; i < (uint32_t)sent; ++i) {
    rudp_conn* c = conn_by_index(ep, ep->send_batch_conn[i]);
    if (c) {
      c->ack_dirty = 0;
      c->last_tx_ns = now_ns;
    }
    record_sent_packet(ep, i, now_ns);
    if (!ep->send_batch_ack_only[i]) {
      uint16_t msg_count = ep->send_batch_msg_counts[i];
      for (uint16_t j = 0; j < msg_count; ++j) {
        uint32_t mi = ep->send_batch_msgs[(size_t)i * RUDP_MAX_REFS_PER_PACKET + j];
        if (mi >= ep->cfg.max_messages) continue;
        rudp_msg* m = &ep->msgs[mi];
        if (!m->used) continue;
        rudp_flow_state* f = c ? flow_for(c, m->flow_id) : NULL;
        if (f) add_u32_saturating(&f->stats.sent_bps, m->len);
        if (msg_is_reliable(m)) {
          continue;
        }
        account_unsent_queue_removed(c, f, m);
        free_msg(ep, mi);
      }
    }
  }
  for (uint32_t i = old_count; i > (uint32_t)sent; --i) {
    requeue_unsent_packet(ep, i - 1u);
  }
  ep->send_count = 0;
  return (uint32_t)sent == old_count;
}

static int sent_slot_available(rudp_conn* c, uint32_t seq) {
  rudp_endpoint* ep = c->ep;
  size_t slot = (size_t)c->index * ep->cfg.sent_packet_count +
                (seq % ep->cfg.sent_packet_count);
  rudp_sent_packet* sp = &ep->sent_packets[slot];
  if (sp->used && sp->conn_index == c->index) return 0;
  for (uint32_t i = 0; i < ep->send_count; ++i) {
    if (ep->send_batch_ref_counts[i] == 0 ||
        ep->send_batch_conn[i] != c->index) {
      continue;
    }
    uint32_t pending_seq = load_u32(ep->send_batch[i].data + 12);
    if ((pending_seq % ep->cfg.sent_packet_count) ==
        (seq % ep->cfg.sent_packet_count)) {
      return 0;
    }
  }
  return 1;
}

static uint32_t next_nonzero_packet_seq(rudp_conn* c) {
  uint32_t seq = c->next_packet_seq++;
  if (seq == 0) seq = c->next_packet_seq++;
  return seq ? seq : 1u;
}

/* ACK bitmap は max_seq 相対 64bit しかないので、un-ACK の最古 seq から
   RUDP_ACK_WINDOW 以上先の seq を発行すると受信側が SACK できず不要再送に
   なる。reliable パケットの発行を SACK 幅以内にゲートして整合させる。 */
#define RUDP_ACK_WINDOW 64u

static int reliable_window_allows(rudp_conn* c, uint32_t candidate) {
  rudp_endpoint* ep = c->ep;
  if (ep->sent_seq_queue && c->sent_seq_count > 0) prune_sent_seq_queue(c);
  uint32_t oldest = 0;
  int have_oldest = 0;
  if (ep->sent_seq_queue && c->sent_seq_count > 0) {
    oldest = ep->sent_seq_queue[(size_t)c->index * ep->cfg.sent_packet_count +
                                c->sent_seq_head];
    have_oldest = 1;
  } else if (c->batch_reliable_first_seq != 0) {
    /* まだ record されていない同一 flush 内の発行分もウィンドウに数える。 */
    oldest = c->batch_reliable_first_seq;
    have_oldest = 1;
  }
  if (!have_oldest) return 1;
  return (uint32_t)(candidate - oldest) < RUDP_ACK_WINDOW;
}

static int next_packet_seq(rudp_conn* c, int needs_tracking, uint32_t* out_seq) {
  if (!needs_tracking) {
    if (c->ep->cfg.skip_unreliable_acks) {
      /* unreliable 専用パケットは seq を消費しない（seq=0 で送る）。
         seq 空間を reliable パケット専用にしないと、unreliable が seq を
         先へ進めて reliable 同士の SACK bitmap 距離（raw seq 差）を引き
         伸ばし、64 幅のウィンドウを押し流してしまう。 */
      *out_seq = 0;
      return 1;
    }
    /* skip_unreliable_acks=0 のモードでは unreliable も ACK/reorder 統計の
       対象なので従来どおり seq を振る（このモードは SACK 窓の希釈と
       引き換え）。 */
    *out_seq = next_nonzero_packet_seq(c);
    return 1;
  }
  {
    uint32_t candidate = c->next_packet_seq ? c->next_packet_seq : 1u;
    if (!reliable_window_allows(c, candidate)) return 0;
  }
  uint32_t tries = c->ep->cfg.sent_packet_count;
  while (tries-- > 0) {
    uint32_t seq = next_nonzero_packet_seq(c);
    if (sent_slot_available(c, seq)) {
      if (c->batch_reliable_first_seq == 0) c->batch_reliable_first_seq = seq;
      *out_seq = seq;
      return 1;
    }
  }
  return 0;
}

static int batch_packet_can_add_reliable(rudp_endpoint* ep, uint32_t batch_index) {
  if (ep->send_batch_ref_counts[batch_index] > 0) return 1;
  rudp_conn* c = conn_by_index(ep, ep->send_batch_conn[batch_index]);
  if (!c) return 0;
  const uint8_t* pkt = ep->send_batch[batch_index].data;
  uint32_t seq = load_u32(pkt + 12);
  /* seq=0（unreliable 専用）のパケットには reliable を相乗りさせない。
     追跡 seq が無いので ACK/再送管理ができない。 */
  if (seq == 0) return 0;
  return sent_slot_available(c, seq);
}

static int append_msg_to_batch_packet(rudp_endpoint* ep, uint32_t batch_index,
                                      uint32_t mi, uint8_t source) {
  rudp_msg* m = &ep->msgs[mi];
  uint16_t frame_len = (uint16_t)(RUDP_DATA_FRAME_HEADER_BYTES + m->len);
  if (ep->send_batch[batch_index].len + frame_len > ep->mtu) return 0;
  if (ep->send_batch_msg_counts[batch_index] >= RUDP_MAX_REFS_PER_PACKET) return 0;
  if (msg_is_reliable(m) && !batch_packet_can_add_reliable(ep, batch_index)) return 0;
  uint8_t* pkt = ep->send_batch_data + ((size_t)batch_index * ep->mtu);
  size_t offset = ep->send_batch[batch_index].len;
  write_data_frame_header(pkt + offset, m);
  if (m->len > 0) memcpy(pkt + offset + RUDP_DATA_FRAME_HEADER_BYTES, m->data, m->len);
  ep->send_batch[batch_index].len += frame_len;
  uint16_t payload_len = (uint16_t)(ep->send_batch[batch_index].len - RUDP_PACKET_HEADER_BYTES);
  store_u16(pkt + 30, payload_len);
  uint16_t msg_slot = ep->send_batch_msg_counts[batch_index]++;
  ep->send_batch_msgs[(size_t)batch_index * RUDP_MAX_REFS_PER_PACKET + msg_slot] = mi;
  ep->send_batch_msg_sources[(size_t)batch_index * RUDP_MAX_REFS_PER_PACKET + msg_slot] = source;
  if (msg_is_reliable(m)) {
    uint16_t ref_slot = ep->send_batch_ref_counts[batch_index]++;
    ep->send_batch_refs[(size_t)batch_index * RUDP_MAX_REFS_PER_PACKET + ref_slot] = mi;
  }
  return 1;
}

static int try_append_to_previous_packet(rudp_conn* c, uint32_t mi, uint8_t source) {
  rudp_endpoint* ep = c->ep;
  if (ep->send_count == 0) return 0;
  uint32_t idx = ep->send_count - 1u;
  if (ep->send_batch_ack_only[idx] || ep->send_batch_conn[idx] != c->index) return 0;
  return append_msg_to_batch_packet(ep, idx, mi, source);
}

static int append_packet_for_msg(rudp_conn* c, uint32_t mi, uint8_t source) {
  rudp_endpoint* ep = c->ep;
  if (try_append_to_previous_packet(c, mi, source)) {
    return 1;
  }
  if (ep->send_count >= ep->cfg.send_batch_size) return 0;
  uint8_t* pkt = ep->send_batch_data + ((size_t)ep->send_count * ep->mtu);
  rudp_msg* m = &ep->msgs[mi];
  uint32_t seq = 0;
  if (!next_packet_seq(c, msg_is_reliable(m), &seq)) return 0;
  write_packet_header(pkt, 0, c, seq, 0);
  ep->send_batch[ep->send_count].addr = c->peer;
  ep->send_batch[ep->send_count].data = pkt;
  ep->send_batch[ep->send_count].len = RUDP_PACKET_HEADER_BYTES;
  ep->send_batch_conn[ep->send_count] = c->index;
  ep->send_batch_ref_counts[ep->send_count] = 0;
  ep->send_batch_msg_counts[ep->send_count] = 0;
  ep->send_batch_ack_only[ep->send_count] = 0;
  if (!append_msg_to_batch_packet(ep, ep->send_count, mi, source)) return 0;
  ++ep->send_count;
  return 1;
}

static void append_ack_only(rudp_conn* c, uint64_t now_ns) {
  rudp_endpoint* ep = c->ep;
  if (!c->ack_dirty) return;
  if (ep->send_count >= ep->cfg.send_batch_size &&
      !flush_send_batch(ep, now_ns)) {
    return;
  }
  if (ep->send_count >= ep->cfg.send_batch_size) return;
  uint8_t* pkt = ep->send_batch_data + ((size_t)ep->send_count * ep->mtu);
  uint32_t seq = 0;
  write_packet_header(pkt, RUDP_PACKET_FLAG_ACK_ONLY, c, seq, 0);
  ep->send_batch[ep->send_count].addr = c->peer;
  ep->send_batch[ep->send_count].data = pkt;
  ep->send_batch[ep->send_count].len = RUDP_PACKET_HEADER_BYTES;
  ep->send_batch_conn[ep->send_count] = c->index;
  ep->send_batch_ref_counts[ep->send_count] = 0;
  ep->send_batch_msg_counts[ep->send_count] = 0;
  ep->send_batch_ack_only[ep->send_count] = 1;
  ++ep->send_count;
}

static int conn_idle_expired(const rudp_conn* c, uint64_t now_ns) {
  if (c->ep->cfg.idle_timeout_ms == 0 || c->last_rx_ns == 0) return 0;
  uint64_t idle_ns = (uint64_t)c->ep->cfg.idle_timeout_ms * RUDP_NS_PER_MS;
  return now_ns >= add_ns_saturating(c->last_rx_ns, idle_ns);
}

/* pacing トークンの補充。バーストは max(16*mtu, 10ms 分) まで。
   無制限（UINT32_MAX）の間はトークンを満杯に保ち、loss で有限レートに
   遷移した直後も 1 バースト分は即時送信できるようにする（0 から始めると
   同一時刻内の再送がストールする）。 */
static void refill_pace_tokens(rudp_conn* c, uint64_t now_ns) {
  if (c->pacing_bps == UINT32_MAX) {
    c->pace_tokens_bytes = UINT64_MAX;
    c->pace_refill_ns = now_ns;
    return;
  }
  uint64_t rate_bytes_per_sec = (uint64_t)c->pacing_bps / 8u;
  uint64_t burst_cap = 16u * (uint64_t)c->ep->mtu;
  uint64_t ten_ms_bytes = rate_bytes_per_sec / 100u;
  if (ten_ms_bytes > burst_cap) burst_cap = ten_ms_bytes;
  if (c->pace_tokens_bytes > burst_cap) c->pace_tokens_bytes = burst_cap;
  if (c->pace_refill_ns == 0 || now_ns <= c->pace_refill_ns) {
    c->pace_refill_ns = now_ns;
    return;
  }
  uint64_t elapsed = now_ns - c->pace_refill_ns;
  if (elapsed > RUDP_NS_PER_SEC) elapsed = RUDP_NS_PER_SEC;
  c->pace_tokens_bytes += rate_bytes_per_sec * elapsed / RUDP_NS_PER_SEC;
  if (c->pace_tokens_bytes > burst_cap) c->pace_tokens_bytes = burst_cap;
  c->pace_refill_ns = now_ns;
}

static int pace_allows_send(const rudp_conn* c) {
  if (c->pacing_bps == UINT32_MAX) return 1;
  return c->pace_tokens_bytes >= (uint64_t)c->ep->mtu;
}

static void pace_consume(rudp_conn* c, uint64_t bytes) {
  if (c->pacing_bps == UINT32_MAX) return;
  c->pace_tokens_bytes =
      c->pace_tokens_bytes > bytes ? c->pace_tokens_bytes - bytes : 0;
}

void rudp_endpoint_flush(rudp_endpoint* ep, uint64_t now_ns) {
  if (!ep) return;
  ep->now_ns = now_ns;
  ep->send_count = 0;
  for (uint32_t i = 0; i < ep->cfg.max_conns; ++i) {
    rudp_conn* c = &ep->conns[i];
    if (!c->active) continue;
    if (conn_idle_expired(c, now_ns)) {
      abort_conn_internal(c);
      continue;
    }
    service_retransmits(c, now_ns);
    if (!c->active) continue;
    refill_pace_tokens(c, now_ns);
    c->batch_reliable_first_seq = 0;
    uint32_t mi;
    uint8_t source = RUDP_QUEUE_NONE;
    uint32_t guard = ep->cfg.max_messages;
    int sent_data = 0;
    while (guard-- > 0) {
      if (!pace_allows_send(c)) break;
      if (ep->send_count >= ep->cfg.send_batch_size &&
          !flush_send_batch(ep, now_ns)) {
        break;
      }
      if (!pick_next_msg(c, now_ns, &mi, &source)) break;
      if (!append_packet_for_msg(c, mi, source)) {
        requeue_unsent_msg(c, mi, source);
        break;
      }
      pace_consume(c, (uint64_t)ep->msgs[mi].len +
                          RUDP_DATA_FRAME_HEADER_BYTES);
      sent_data = 1;
    }
    if (!sent_data) append_ack_only(c, now_ns);
  }
  (void)flush_send_batch(ep, now_ns);
}

static rudp_conn* find_or_create_incoming_conn(rudp_endpoint* ep, uint64_t conn_id,
                                               const rudp_addr* peer) {
  if (!addr_is_valid(peer)) return NULL;
  rudp_conn* c = rudp_endpoint_find_conn(ep, conn_id);
  if (c) {
    return addr_equal(&c->peer, peer) ? c : NULL;
  }
  /* ハンドシェイク/cookie が無いので、spoof された conn_id での conn テーブル
     枯渇はこのレート制限（cfg.max_incoming_conns_per_poll、0=無制限）と
     idle timeout で緩和するのみ。 */
  if (ep->cfg.max_incoming_conns_per_poll > 0 &&
      ep->incoming_conns_this_poll >= ep->cfg.max_incoming_conns_per_poll) {
    return NULL;
  }
  for (uint32_t i = 0; i < ep->cfg.max_conns; ++i) {
    if (!ep->conns[i].active) {
      if (create_conn_at(ep, i, peer, conn_id, &c) == 0) {
        ++ep->incoming_conns_this_poll;
        return c;
      }
      return NULL;
    }
  }
  return NULL;
}

static int reliable_seen_lookup(rudp_conn* c, uint32_t msg_id) {
  if (msg_id == 0) return 0;
  if (c->rx_dedup_max != 0 && seq32_after(c->rx_dedup_max, msg_id) &&
      c->rx_dedup_max - msg_id >= RUDP_RX_DEDUP_WINDOW) {
    /* ウィンドウより古い msg_id は判定できないので配信済み扱い（重複抑止）。
       送信側の in-flight は msg pool で有界なので、正常系ではここまで古い
       未配信 msg は到達しない。 */
    return 1;
  }
  uint32_t block = msg_id >> 6u;
  uint32_t w = block & (RUDP_RX_DEDUP_WORDS - 1u);
  if (c->rx_dedup_block[w] != block) return 0;
  return (c->rx_dedup_bits[w] & (1ull << (msg_id & 63u))) != 0;
}

static void reliable_mark_seen(rudp_conn* c, uint32_t msg_id) {
  if (msg_id == 0) return;
  uint32_t block = msg_id >> 6u;
  uint32_t w = block & (RUDP_RX_DEDUP_WORDS - 1u);
  if (c->rx_dedup_block[w] != block) {
    c->rx_dedup_block[w] = block;
    c->rx_dedup_bits[w] = 0;
  }
  c->rx_dedup_bits[w] |= 1ull << (msg_id & 63u);
  if (c->rx_dedup_max == 0 || seq32_after(msg_id, c->rx_dedup_max)) {
    c->rx_dedup_max = msg_id;
  }
}

static int hold_ordered(rudp_conn* c, uint16_t flow_id, uint16_t channel_id,
                        uint16_t channel_seq, uint32_t msg_id,
                        rudp_reliability reliability, const uint8_t* data,
                        uint16_t len) {
  rudp_endpoint* ep = c->ep;
  if (len > ep->max_payload) return 0;
  if (ep->ordered_hold_count > 0) {
    for (uint32_t i = 0; i < ep->cfg.max_ordered_holds; ++i) {
      rudp_ordered_hold* h = &ep->holds[i];
      if (h->used && h->conn_index == c->index &&
          h->channel_id == channel_id &&
          h->channel_seq == channel_seq) {
        return h->msg_id == msg_id;
      }
    }
  }
  for (uint32_t i = 0; i < ep->cfg.max_ordered_holds; ++i) {
    if (ep->holds[i].used) continue;
    rudp_ordered_hold* h = &ep->holds[i];
    h->used = 1;
    h->conn_index = c->index;
    h->msg_id = msg_id;
    h->flow_id = flow_id;
    h->channel_id = channel_id;
    h->channel_seq = channel_seq;
    h->reliability = reliability;
    h->len = len;
    h->data = ep->hold_data + ((size_t)i * ep->max_payload);
    if (len > 0) memcpy(h->data, data, len);
    ++ep->ordered_hold_count;
    return 1;
  }
  return 0;
}

static void drain_ordered(rudp_conn* c, uint16_t channel_id) {
  rudp_endpoint* ep = c->ep;
  /* Runs per delivered message; with no held ordered data the scan below is
     a guaranteed miss over max_ordered_holds slots. */
  if (ep->ordered_hold_count == 0) return;
  rudp_channel_state* ch = channel_for(c, channel_id);
  if (!ch) return;
  int progressed = 1;
  while (progressed) {
    progressed = 0;
    for (uint32_t i = 0; i < ep->cfg.max_ordered_holds; ++i) {
      rudp_ordered_hold* h = &ep->holds[i];
      if (!h->used || h->conn_index != c->index || h->channel_id != channel_id ||
          h->channel_seq != ch->expected_ordered) {
        continue;
      }
      if (!enqueue_recv(c, h->flow_id, h->channel_id, h->reliability,
                        h->data, h->len, 0)) {
        return;
      }
      h->used = 0;
      if (ep->ordered_hold_count > 0) --ep->ordered_hold_count;
      ++ch->expected_ordered;
      progressed = 1;
      break;
    }
  }
}

static void drain_all_ordered(rudp_conn* c) {
  if (!c) return;
  /* hold が無ければ全チャネル走査をスキップ（recv 毎に呼ばれるホットパス）。 */
  if (c->ep->ordered_hold_count == 0) return;
  for (uint32_t i = 0; i < c->ep->cfg.max_channels; ++i) {
    if (c->ep->recv_count >= c->ep->cfg.max_recv_events) return;
    drain_ordered(c, (uint16_t)i);
  }
}

static rudp_reliability reliability_from_flags(uint8_t flags) {
  if (flags & RUDP_F_RELIABLE) {
    return (flags & RUDP_F_ORDERED) ? RUDP_RELIABLE_ORDERED : RUDP_RELIABLE_UNORDERED;
  }
  if (flags & RUDP_F_SEQUENCED) return RUDP_UNRELIABLE_SEQUENCED;
  return RUDP_UNRELIABLE;
}

static int frame_flags_are_valid(uint8_t flags) {
  const uint8_t known = RUDP_F_RELIABLE | RUDP_F_ORDERED | RUDP_F_SEQUENCED;
  if ((flags & (uint8_t)~known) != 0) return 0;
  if ((flags & RUDP_F_ORDERED) && !(flags & RUDP_F_RELIABLE)) return 0;
  if ((flags & RUDP_F_SEQUENCED) &&
      (flags & (RUDP_F_RELIABLE | RUDP_F_ORDERED))) {
    return 0;
  }
  return 1;
}

static int deliver_frame(rudp_conn* c, uint8_t flags, uint16_t flow_id,
                         uint16_t payload_len, uint32_t msg_id,
                         uint16_t channel_id, uint16_t channel_seq,
                         const uint8_t* payload, int borrow) {
  if (flow_id >= c->ep->cfg.max_flows || channel_id >= c->ep->cfg.max_channels) return 0;
  if (flags == 0) {
    return enqueue_recv(c, flow_id, channel_id, RUDP_UNRELIABLE, payload,
                        payload_len, borrow);
  }
  rudp_reliability rel = reliability_from_flags(flags);
  rudp_channel_state* ch = channel_for(c, channel_id);
  if (!ch) return 0;
  if ((rel == RUDP_RELIABLE_UNORDERED || rel == RUDP_RELIABLE_ORDERED) &&
      reliable_seen_lookup(c, msg_id)) {
    return 1;
  }
  if (rel == RUDP_UNRELIABLE_SEQUENCED) {
    rudp_sequenced_state* seq = sequenced_for(c, flow_id, channel_id);
    if (!seq) return 0;
    if (seq->have_latest_sequenced &&
        !seq16_after(channel_seq, seq->latest_sequenced)) {
      return 1;
    }
    seq->have_latest_sequenced = 1;
    seq->latest_sequenced = channel_seq;
    (void)enqueue_recv(c, flow_id, channel_id, rel, payload, payload_len, borrow);
    return 1;
  }
  if (rel == RUDP_RELIABLE_ORDERED) {
    if (channel_seq != ch->expected_ordered &&
        !seq16_after(channel_seq, ch->expected_ordered)) {
      return 1;
    }
    if (channel_seq != ch->expected_ordered) {
      if (!hold_ordered(c, flow_id, channel_id, channel_seq, msg_id, rel,
                        payload, payload_len)) {
        return 0;
      }
      reliable_mark_seen(c, msg_id);
      return 1;
    }
    if (!enqueue_recv(c, flow_id, channel_id, rel, payload, payload_len, borrow)) {
      return 0;
    }
    reliable_mark_seen(c, msg_id);
    ++ch->expected_ordered;
    drain_ordered(c, channel_id);
    return 1;
  }
  if (!enqueue_recv(c, flow_id, channel_id, rel, payload, payload_len, borrow)) {
    return 0;
  }
  reliable_mark_seen(c, msg_id);
  return 1;
}

static int reasm_has_frag(const rudp_rx_reasm* r, uint16_t frag_index) {
  uint16_t word = (uint16_t)(frag_index / 64u);
  uint16_t bit = (uint16_t)(frag_index % 64u);
  return (r->received[word] & (1ull << bit)) != 0;
}

static void reasm_set_frag(rudp_rx_reasm* r, uint16_t frag_index) {
  uint16_t word = (uint16_t)(frag_index / 64u);
  uint16_t bit = (uint16_t)(frag_index % 64u);
  r->received[word] |= (1ull << bit);
}

static rudp_rx_reasm* find_or_alloc_reasm(rudp_conn* c, uint32_t msg_id,
                                          uint16_t flow_id, uint16_t channel_id,
                                          uint16_t channel_seq, uint16_t frag_count,
                                          rudp_reliability reliability,
                                          uint64_t now_ns) {
  rudp_endpoint* ep = c->ep;
  rudp_rx_reasm* free_slot = NULL;
  for (uint32_t i = 0; i < ep->reasm_count; ++i) {
    rudp_rx_reasm* r = &ep->reasm[i];
    if (!r->used) {
      if (!free_slot) free_slot = r;
      continue;
    }
    if (r->conn_index == c->index && r->msg_id == msg_id) return r;
  }
  if (!free_slot) return NULL;
  uint32_t idx = (uint32_t)(free_slot - ep->reasm);
  memset(free_slot, 0, sizeof(*free_slot));
  free_slot->used = 1;
  free_slot->conn_index = c->index;
  free_slot->msg_id = msg_id;
  uint64_t ttl_ns = (uint64_t)ep->cfg.rto_ms * 4ull * RUDP_NS_PER_MS;
  if (ttl_ns < 100ull * RUDP_NS_PER_MS) ttl_ns = 100ull * RUDP_NS_PER_MS;
  free_slot->deadline_ns = add_ns_saturating(now_ns, ttl_ns);
  free_slot->flow_id = flow_id;
  free_slot->channel_id = channel_id;
  free_slot->channel_seq = channel_seq;
  free_slot->frag_count = frag_count;
  free_slot->reliability = reliability;
  free_slot->data = ep->reasm_data + ((size_t)idx * ep->max_payload);
  return free_slot;
}

static void expire_reassembly(rudp_endpoint* ep, uint64_t now_ns) {
  for (uint32_t i = 0; i < ep->reasm_count; ++i) {
    rudp_rx_reasm* r = &ep->reasm[i];
    if (r->used && r->deadline_ns && now_ns >= r->deadline_ns) {
      r->used = 0;
    }
  }
}

static int process_data_frame(rudp_conn* c, uint8_t flags, uint16_t flow_id,
                              uint16_t payload_len, uint32_t msg_id,
                              uint16_t frag_index, uint16_t frag_count,
                              uint16_t channel_id, uint16_t channel_seq,
                              const uint8_t* payload, uint64_t now_ns) {
  if (frag_count == 0 || frag_index >= frag_count ||
      frag_count > RUDP_MAX_FRAGS_PER_MSG) {
    return 0;
  }
  rudp_reliability rel = reliability_from_flags(flags);
  int reliable = rel == RUDP_RELIABLE_UNORDERED || rel == RUDP_RELIABLE_ORDERED;
  if (frag_count == 1) {
    if (payload_len > c->ep->frame_payload || payload_len > c->ep->max_payload) {
      return 0;
    }
    int delivered = deliver_frame(c, flags, flow_id, payload_len, msg_id,
                                  channel_id, channel_seq, payload, 1);
    return delivered || !reliable;
  }
  size_t offset = (size_t)frag_index * c->ep->frame_payload;
  if (offset + payload_len > c->ep->max_payload) return 0;
  if (reliable && reliable_seen_lookup(c, msg_id)) {
    return 1;
  }
  rudp_rx_reasm* r = find_or_alloc_reasm(c, msg_id, flow_id, channel_id,
                                         channel_seq, frag_count, rel, now_ns);
  if (!r) {
    return !reliable;
  }
  if (r->frag_count != frag_count || r->flow_id != flow_id ||
      r->channel_id != channel_id || r->channel_seq != channel_seq ||
      r->reliability != rel) {
    return 0;
  }
  if (reasm_has_frag(r, frag_index)) {
    if (r->received_count == r->frag_count) {
      int delivered = deliver_frame(c, flags, r->flow_id, r->total_len,
                                    r->msg_id, r->channel_id,
                                    r->channel_seq, r->data, 0);
      if (delivered || !reliable) r->used = 0;
      return delivered || !reliable;
    }
    return 1;
  }
  if (payload_len > 0) memcpy(r->data + offset, payload, payload_len);
  reasm_set_frag(r, frag_index);
  ++r->received_count;
  size_t end = offset + payload_len;
  if (end > r->total_len) r->total_len = (uint16_t)end;
  if (r->received_count == r->frag_count) {
    int delivered = deliver_frame(c, flags, r->flow_id, r->total_len,
                                  r->msg_id, r->channel_id, r->channel_seq,
                                  r->data, 0);
    if (delivered || !reliable) r->used = 0;
    return delivered || !reliable;
  }
  return 1;
}

static int validate_packet_frames(rudp_endpoint* ep, const uint8_t* p,
                                  size_t off, size_t end, int allow_reliable) {
  while (off < end) {
    if (off + RUDP_DATA_FRAME_HEADER_BYTES > end) return 0;
    const uint8_t* f = p + off;
    uint8_t type = f[0];
    uint8_t flags = f[1];
    /* seq=0 のパケット（unreliable 専用）に reliable フレームは載らない。
       packet seq が無いと ACK/再送管理ができないため受信側でも弾く。 */
    if (!allow_reliable && (flags & RUDP_F_RELIABLE)) return 0;
    uint16_t frame_header_len = load_u16(f + 2);
    uint16_t flow_id = load_u16(f + 4);
    uint16_t frame_payload_len = load_u16(f + 6);
    uint16_t frag_index = load_u16(f + 12);
    uint16_t frag_count = load_u16(f + 14);
    uint16_t channel_id = load_u16(f + 16);
    if (type != RUDP_FRAME_DATA ||
        !frame_flags_are_valid(flags) ||
        frame_header_len != RUDP_DATA_FRAME_HEADER_BYTES ||
        off + frame_header_len + frame_payload_len > end) {
      return 0;
    }
    if (flow_id >= ep->cfg.max_flows || channel_id >= ep->cfg.max_channels ||
        frag_count == 0 || frag_index >= frag_count ||
        frag_count > RUDP_MAX_FRAGS_PER_MSG) {
      return 0;
    }
    if (frag_count == 1) {
      if (frag_index != 0 || frame_payload_len > ep->frame_payload ||
          frame_payload_len > ep->max_payload) {
        return 0;
      }
    } else {
      uint16_t max_frags = (uint16_t)((ep->max_payload + ep->frame_payload - 1u) /
                                      ep->frame_payload);
      if (frag_count > max_frags) return 0;
      size_t frag_off = (size_t)frag_index * ep->frame_payload;
      if (frame_payload_len == 0 || frame_payload_len > ep->frame_payload ||
          frag_off + frame_payload_len > ep->max_payload) {
        return 0;
      }
      if (frag_index + 1u < frag_count &&
          frame_payload_len != ep->frame_payload) {
        return 0;
      }
    }
    off += frame_header_len + frame_payload_len;
  }
  return off == end;
}

int rudp_packet_precheck(const void* data, size_t len, uint16_t mtu,
                         uint64_t* out_conn_id) {
  const uint8_t* p = (const uint8_t*)data;
  if (!p && len != 0) return 0;
  if (len < RUDP_PACKET_HEADER_BYTES) return 0;
  if (mtu != 0 && len > mtu) return 0;
  if ((load_u32(p + 0) & RUDP_PACKET_MAGIC_MASK) != RUDP_PACKET_MAGIC) return 0;
  if (load_u16(p + 28) != RUDP_PACKET_HEADER_BYTES) return 0;
  if (out_conn_id) *out_conn_id = load_u64(p + 4);
  return 1;
}

static void process_packet(rudp_endpoint* ep, const rudp_in_packet* in, uint64_t now_ns) {
  if (!in || !in->data || in->len > in->cap ||
      in->len < RUDP_PACKET_HEADER_BYTES || in->len > ep->mtu) {
    return;
  }
  const uint8_t* p = in->data;
  uint32_t magic_flags = load_u32(p + 0);
  if ((magic_flags & RUDP_PACKET_MAGIC_MASK) != RUDP_PACKET_MAGIC) return;
  uint8_t packet_flags = (uint8_t)(magic_flags & 0xffu);
  if ((packet_flags & (uint8_t)~RUDP_PACKET_FLAG_ACK_ONLY) != 0) return;
  uint64_t conn_id = load_u64(p + 4);
  uint32_t packet_seq = load_u32(p + 12);
  uint32_t ack_seq = load_u32(p + 16);
  uint64_t ack_bits = load_u64(p + 20);
  uint16_t header_len = load_u16(p + 28);
  uint16_t payload_len = load_u16(p + 30);
  if (header_len != RUDP_PACKET_HEADER_BYTES ||
      (size_t)header_len + payload_len != in->len) {
    return;
  }

  size_t off = header_len;
  size_t end = header_len + payload_len;
  int ack_only = (packet_flags & RUDP_PACKET_FLAG_ACK_ONLY) != 0;
  if (ack_only) {
    if (packet_seq != 0 || payload_len != 0) return;
  } else if (payload_len == 0 ||
             !validate_packet_frames(ep, p, off, end,
                                     /*allow_reliable=*/packet_seq != 0)) {
    /* データパケットの packet_seq==0 は「unreliable 専用（ACK 不要）」を
       意味するので、unreliable フレームのみなら許容する。 */
    return;
  }

  rudp_conn* c = ack_only ? rudp_endpoint_find_conn(ep, conn_id)
                          : find_or_create_incoming_conn(ep, conn_id, &in->addr);
  if (!c) return;
  if (ack_only && !addr_is_valid(&in->addr)) return;
  if (ack_only && !addr_equal(&c->peer, &in->addr)) return;
  c->last_rx_ns = now_ns;
  c->retx_rounds_since_rx = 0;
  c->rto_backoff = 0;
  if (ack_only) {
    if (ack_seq != 0) release_acked_packets(c, ack_seq, ack_bits, now_ns);
    return;
  }

  int packet_has_reliable = 0;
  int packet_failed = 0;
  while (off < end) {
    const uint8_t* f = p + off;
    uint8_t type = f[0];
    uint8_t flags = f[1];
    uint16_t frame_header_len = load_u16(f + 2);
    uint16_t flow_id = load_u16(f + 4);
    uint16_t frame_payload_len = load_u16(f + 6);
    uint32_t msg_id = load_u32(f + 8);
    uint16_t frag_index = load_u16(f + 12);
    uint16_t frag_count = load_u16(f + 14);
    uint16_t channel_id = load_u16(f + 16);
    uint16_t channel_seq = load_u16(f + 18);
    if (type != RUDP_FRAME_DATA || frame_header_len != RUDP_DATA_FRAME_HEADER_BYTES ||
        off + frame_header_len + frame_payload_len > end) {
      return;
    }
    if (flags & RUDP_F_RELIABLE) packet_has_reliable = 1;
    if (!process_data_frame(c, flags, flow_id, frame_payload_len, msg_id,
                            frag_index, frag_count, channel_id, channel_seq,
                            p + off + frame_header_len, now_ns)) {
      packet_failed = 1;
    }
    (void)now_ns;
    off += frame_header_len + frame_payload_len;
  }
  if (!packet_failed && ack_seq != 0) {
    release_acked_packets(c, ack_seq, ack_bits, now_ns);
  }
  if (packet_seq != 0 &&
      !packet_failed &&
      (!ep->cfg.skip_unreliable_acks || packet_has_reliable)) {
    int reordered = c->ack.max_seq != 0 && packet_seq != c->ack.max_seq &&
                    !seq32_after(packet_seq, c->ack.max_seq);
    if (ack_insert(&c->ack, packet_seq)) {
      ++c->recv_packets;
      if (reordered) ++c->reordered_packets;
      c->ack_dirty = 1;
    }
  }
}

static void reset_recv_batch_slot(rudp_endpoint* ep, uint32_t i) {
  ep->recv_batch[i].addr.len = 0;
  ep->recv_batch[i].data = ep->recv_batch_data + ((size_t)i * ep->mtu);
  ep->recv_batch[i].cap = ep->mtu;
  ep->recv_batch[i].len = 0;
}

void rudp_endpoint_poll(rudp_endpoint* ep, uint64_t now_ns) {
  if (!ep) return;
  ep->now_ns = now_ns;
  ep->incoming_conns_this_poll = 0;
  expire_reassembly(ep, now_ns);
  int n = ep->cfg.socket.recv_batch(ep->cfg.socket.user, ep->recv_batch,
                                    ep->cfg.recv_batch_size);
  if (n <= 0) {
    if (n < 0) {
      for (uint32_t i = 0; i < ep->cfg.recv_batch_size; ++i) {
        reset_recv_batch_slot(ep, i);
      }
    }
    return;
  }
  if ((uint32_t)n > ep->cfg.recv_batch_size) n = (int)ep->cfg.recv_batch_size;
  for (int i = 0; i < n; ++i) {
    process_packet(ep, &ep->recv_batch[i], now_ns);
    reset_recv_batch_slot(ep, (uint32_t)i);
  }
}

static void fill_recv_info(rudp_endpoint* ep, const rudp_recv_event_i* e,
                           rudp_recv_info* out_info) {
  if (!out_info) return;
  out_info->conn = conn_by_index(ep, e->conn_index);
  out_info->conn_id = e->conn_id;
  out_info->flow_id = e->flow_id;
  out_info->channel_id = e->channel_id;
  out_info->reliability = e->reliability;
}

int rudp_recv_borrow(rudp_endpoint* ep, const void** data, size_t* out_len,
                     rudp_recv_info* out_info) {
  if (!ep || !data || !out_len) return -1;
  if (ep->recv_count == 0) return 0;
  rudp_recv_event_i* e = &ep->recv_events[ep->recv_head];
  if (!e->used) return 0;
  if (e->len > 0) memcpy(ep->borrow_data, e->data, e->len);
  *data = ep->borrow_data;
  *out_len = e->len;
  fill_recv_info(ep, e, out_info);
  uint32_t conn_index = e->conn_index;
  e->used = 0;
  ep->recv_head = (ep->recv_head + 1u) % ep->cfg.max_recv_events;
  --ep->recv_count;
  drain_all_ordered(conn_by_index(ep, conn_index));
  return 1;
}

int rudp_recv_borrow_meta(rudp_endpoint* ep, const void** data, size_t* out_len,
                          uint64_t* out_conn_id,
                          rudp_reliability* out_reliability) {
  if (!ep || !data || !out_len) return -1;
  if (ep->recv_count == 0) return 0;
  rudp_recv_event_i* e = &ep->recv_events[ep->recv_head];
  if (!e->used) return 0;
  if (e->len > 0) memcpy(ep->borrow_data, e->data, e->len);
  *data = ep->borrow_data;
  *out_len = e->len;
  if (out_conn_id) *out_conn_id = e->conn_id;
  if (out_reliability) *out_reliability = e->reliability;
  uint32_t conn_index = e->conn_index;
  e->used = 0;
  ep->recv_head = (ep->recv_head + 1u) % ep->cfg.max_recv_events;
  --ep->recv_count;
  drain_all_ordered(conn_by_index(ep, conn_index));
  return 1;
}

int rudp_recv(rudp_endpoint* ep, void* data, size_t cap, size_t* out_len,
              rudp_recv_info* out_info) {
  if (!ep || !out_len || (!data && cap != 0)) return -1;
  if (ep->recv_count == 0) return 0;
  rudp_recv_event_i* e = &ep->recv_events[ep->recv_head];
  if (!e->used) return 0;
  if (e->len > cap) {
    *out_len = e->len;
    fill_recv_info(ep, e, out_info);
    return -1;
  }
  if (e->len > 0) memcpy(data, e->data, e->len);
  *out_len = e->len;
  fill_recv_info(ep, e, out_info);
  uint32_t conn_index = e->conn_index;
  uint16_t channel_id = e->channel_id;
  e->used = 0;
  ep->recv_head = (ep->recv_head + 1u) % ep->cfg.max_recv_events;
  --ep->recv_count;
  (void)channel_id;
  drain_all_ordered(conn_by_index(ep, conn_index));
  return 1;
}
