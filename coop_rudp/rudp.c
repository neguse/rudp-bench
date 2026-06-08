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

#define RUDP_PACKET_HEADER_BYTES 32u
#define RUDP_DATA_FRAME_HEADER_BYTES 20u
#define RUDP_INVALID_INDEX UINT32_MAX
#define RUDP_MAX_REFS_PER_PACKET 16u
#define RUDP_MAX_FRAGS_PER_MSG 256u
#define RUDP_MAX_LOST_ACK_SEQS 4u
#define RUDP_RX_SEEN_BUCKETS 1024u
#define RUDP_RX_SEEN_WAYS 4u
#define RUDP_RX_SEEN_COUNT (RUDP_RX_SEEN_BUCKETS * RUDP_RX_SEEN_WAYS)
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
  uint32_t rx_seen[RUDP_RX_SEEN_COUNT];
  uint8_t rx_seen_used[RUDP_RX_SEEN_COUNT];
  uint8_t rx_seen_next[RUDP_RX_SEEN_BUCKETS];
  uint64_t next_retx_ns;
  uint64_t srtt_ns;
  uint64_t min_rtt_ns;
  uint64_t acked_packets;
  uint64_t lost_packets;
  uint64_t recv_packets;
  uint64_t reordered_packets;
  uint32_t inflight_bytes;
  uint32_t send_queue_bytes;
  uint32_t retransmit_queue_bytes;
  uint32_t safe_bps;
  uint32_t pacing_bps;
  uint32_t queue_delay_ms;
  uint32_t sent_seq_head;
  uint32_t sent_seq_tail;
  uint32_t sent_seq_count;
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

  rudp_recv_event_i* recv_events;
  uint8_t* recv_event_data;
  uint8_t* borrow_data;
  uint32_t recv_head;
  uint32_t recv_tail;
  uint32_t recv_count;

  rudp_rx_reasm* reasm;
  uint8_t* reasm_data;
  uint32_t reasm_count;

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
    c->srtt_ns = rtt_ns;
  } else {
    c->srtt_ns = (c->srtt_ns * 7u + rtt_ns) / 8u;
  }
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
  rudp_endpoint* ep = c->ep;
  size_t base = (size_t)c->index * ep->cfg.sent_packet_count;
  for (uint32_t i = 0; i < ep->cfg.sent_packet_count; ++i) {
    rudp_sent_packet* sp = &ep->sent_packets[base + i];
    if (!sp->used || sp->conn_index != c->index) continue;
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

static void release_late_acked_messages(rudp_conn* c, uint32_t ack,
                                        uint64_t bits) {
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

  int changed = 0;
  do {
    changed = 0;
    size_t base = (size_t)c->index * ep->cfg.sent_packet_count;
    for (uint32_t i = 0; i < ep->cfg.sent_packet_count; ++i) {
      rudp_sent_packet* sp = &ep->sent_packets[base + i];
      if (!sp->used || sp->conn_index != c->index) continue;
      for (uint16_t r = 0; r < sp->ref_count; ++r) {
        uint32_t mi = sp->refs[r];
        rudp_msg* m = (mi < ep->cfg.max_messages) ? &ep->msgs[mi] : NULL;
        if (m && m->used && msg_lost_seq_is_acked(m, ack, bits)) {
          complete_late_acked_msg(c, mi, 0);
          changed = 1;
          break;
        }
      }
      if (changed) break;
    }
  } while (changed);
}

static void mark_packet_lost(rudp_conn* c, rudp_sent_packet* sp);

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
        mark_packet_lost(c, sp);
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
      !checked_mul_size(ep->cfg.max_recv_events, ep->max_payload,
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
  ep->reasm_count = ep->cfg.max_recv_events;
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
  return conn_by_index(ep, idx);
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
  uint16_t channel_seq = 0;
  if (opts->reliability == RUDP_RELIABLE_ORDERED) {
    channel_seq = ch->next_ordered_seq++;
  } else if (opts->reliability == RUDP_UNRELIABLE_SEQUENCED) {
    rudp_sequenced_state* seq =
        sequenced_for(conn, opts->flow_id, opts->channel_id);
    if (!seq) {
      ++f->stats.send_queue_full;
      return RUDP_SEND_QUEUE_FULL;
    }
    channel_seq = seq->next_send_seq++;
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
    (void)ring_push_priority(q, conn->ep, allocated[frag]);
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

static void mark_packet_lost(rudp_conn* c, rudp_sent_packet* sp) {
  if (!c || !sp || sp->lost) return;
  sp->lost = 1;
  ++c->lost_packets;
  if (c->safe_bps > 64000u) c->safe_bps /= 2u;
  c->pacing_bps = c->safe_bps;
  for (uint16_t r = 0; r < sp->ref_count; ++r) {
    uint32_t mi = sp->refs[r];
    if (mi >= c->ep->cfg.max_messages) continue;
    rudp_msg* m = &c->ep->msgs[mi];
    if (!m->used) continue;
    note_msg_lost_seq(m, sp->seq);
    if (m->inflight_refs > 0) {
      --m->inflight_refs;
      if (c->inflight_bytes >= m->len) c->inflight_bytes -= m->len;
    }
    if (!queue_retransmit(c, mi)) {
      drop_msg(c, mi, RUDP_QUEUE_NONE, 1);
    }
  }
  sp->used = 0;
}

static void service_retransmits(rudp_conn* c, uint64_t now_ns) {
  if (c->next_retx_ns == 0 || now_ns < c->next_retx_ns) return;
  rudp_endpoint* ep = c->ep;
  uint64_t rto_ns = (uint64_t)ep->cfg.rto_ms * RUDP_NS_PER_MS;
  uint64_t next_retx_ns = 0;
  size_t base = (size_t)c->index * ep->cfg.sent_packet_count;
  for (uint32_t i = 0; i < ep->cfg.sent_packet_count; ++i) {
    rudp_sent_packet* sp = &ep->sent_packets[base + i];
    if (!sp->used || sp->conn_index != c->index || sp->lost) {
      continue;
    }
    uint64_t due = add_ns_saturating(sp->sent_ns, rto_ns);
    if (now_ns < due) {
      if (next_retx_ns == 0 || due < next_retx_ns) next_retx_ns = due;
      continue;
    }
    mark_packet_lost(c, sp);
  }
  c->next_retx_ns = next_retx_ns;
}

static void record_sent_packet(rudp_endpoint* ep, uint32_t batch_index, uint64_t now_ns) {
  uint16_t refs = ep->send_batch_ref_counts[batch_index];
  if (refs == 0) return;
  rudp_conn* c = conn_by_index(ep, ep->send_batch_conn[batch_index]);
  if (!c) return;
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
  uint64_t due = deadline_after_ms(now_ns, ep->cfg.rto_ms);
  if (c->next_retx_ns == 0 || due < c->next_retx_ns) c->next_retx_ns = due;
  for (uint16_t i = 0; i < refs; ++i) {
    uint32_t mi = ep->send_batch_refs[(size_t)batch_index * RUDP_MAX_REFS_PER_PACKET + i];
    sp->refs[i] = mi;
    if (mi >= ep->cfg.max_messages) continue;
    rudp_msg* m = &ep->msgs[mi];
    if (!m->used) continue;
    ++m->inflight_refs;
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
    if (c) c->ack_dirty = 0;
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

static int next_packet_seq(rudp_conn* c, int needs_tracking, uint32_t* out_seq) {
  if (!needs_tracking) {
    *out_seq = next_nonzero_packet_seq(c);
    return 1;
  }
  uint32_t tries = c->ep->cfg.sent_packet_count;
  while (tries-- > 0) {
    uint32_t seq = next_nonzero_packet_seq(c);
    if (sent_slot_available(c, seq)) {
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

void rudp_endpoint_flush(rudp_endpoint* ep, uint64_t now_ns) {
  if (!ep) return;
  ep->now_ns = now_ns;
  ep->send_count = 0;
  for (uint32_t i = 0; i < ep->cfg.max_conns; ++i) {
    rudp_conn* c = &ep->conns[i];
    if (!c->active) continue;
    service_retransmits(c, now_ns);
    uint32_t mi;
    uint8_t source = RUDP_QUEUE_NONE;
    uint32_t guard = ep->cfg.max_messages;
    int sent_data = 0;
    while (guard-- > 0) {
      if (ep->send_count >= ep->cfg.send_batch_size &&
          !flush_send_batch(ep, now_ns)) {
        break;
      }
      if (!pick_next_msg(c, now_ns, &mi, &source)) break;
      if (!append_packet_for_msg(c, mi, source)) {
        requeue_unsent_msg(c, mi, source);
        break;
      }
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
  for (uint32_t i = 0; i < ep->cfg.max_conns; ++i) {
    if (!ep->conns[i].active) {
      if (create_conn_at(ep, i, peer, conn_id, &c) == 0) return c;
      return NULL;
    }
  }
  return NULL;
}

static int reliable_seen_lookup(rudp_conn* c, uint32_t msg_id) {
  uint32_t bucket = (uint32_t)hash_conn_id(msg_id) & (RUDP_RX_SEEN_BUCKETS - 1u);
  uint32_t base = bucket * RUDP_RX_SEEN_WAYS;
  for (uint32_t way = 0; way < RUDP_RX_SEEN_WAYS; ++way) {
    uint32_t idx = base + way;
    if (c->rx_seen_used[idx] && c->rx_seen[idx] == msg_id) return 1;
  }
  return 0;
}

static void reliable_mark_seen(rudp_conn* c, uint32_t msg_id) {
  uint32_t bucket = (uint32_t)hash_conn_id(msg_id) & (RUDP_RX_SEEN_BUCKETS - 1u);
  uint32_t base = bucket * RUDP_RX_SEEN_WAYS;
  for (uint32_t way = 0; way < RUDP_RX_SEEN_WAYS; ++way) {
    uint32_t idx = base + way;
    if (c->rx_seen_used[idx] && c->rx_seen[idx] == msg_id) return;
    if (!c->rx_seen_used[idx]) {
      c->rx_seen_used[idx] = 1;
      c->rx_seen[idx] = msg_id;
      return;
    }
  }
  uint32_t victim = base + c->rx_seen_next[bucket];
  c->rx_seen_next[bucket] =
      (uint8_t)((c->rx_seen_next[bucket] + 1u) % RUDP_RX_SEEN_WAYS);
  c->rx_seen[victim] = msg_id;
}

static int hold_ordered(rudp_conn* c, uint16_t flow_id, uint16_t channel_id,
                        uint16_t channel_seq, uint32_t msg_id,
                        rudp_reliability reliability, const uint8_t* data,
                        uint16_t len) {
  rudp_endpoint* ep = c->ep;
  if (len > ep->max_payload) return 0;
  for (uint32_t i = 0; i < ep->cfg.max_ordered_holds; ++i) {
    rudp_ordered_hold* h = &ep->holds[i];
    if (h->used && h->conn_index == c->index &&
        h->channel_id == channel_id &&
        h->channel_seq == channel_seq) {
      return h->msg_id == msg_id;
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
    return 1;
  }
  return 0;
}

static void drain_ordered(rudp_conn* c, uint16_t channel_id) {
  rudp_endpoint* ep = c->ep;
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
      ++ch->expected_ordered;
      progressed = 1;
      break;
    }
  }
}

static void drain_all_ordered(rudp_conn* c) {
  if (!c) return;
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
                                  size_t off, size_t end) {
  while (off < end) {
    if (off + RUDP_DATA_FRAME_HEADER_BYTES > end) return 0;
    const uint8_t* f = p + off;
    uint8_t type = f[0];
    uint8_t flags = f[1];
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
  } else if (packet_seq == 0 || payload_len == 0 ||
             !validate_packet_frames(ep, p, off, end)) {
    return;
  }

  rudp_conn* c = ack_only ? rudp_endpoint_find_conn(ep, conn_id)
                          : find_or_create_incoming_conn(ep, conn_id, &in->addr);
  if (!c) return;
  if (ack_only && !addr_is_valid(&in->addr)) return;
  if (ack_only && !addr_equal(&c->peer, &in->addr)) return;
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
