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
#define RUDP_REASM_BITMAP_WORDS (RUDP_MAX_FRAGS_PER_MSG / 64u)
#define RUDP_NS_PER_SEC 1000000000ull
#define RUDP_NS_PER_MS 1000000ull

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
  uint8_t inflight;
  rudp_reliability reliability;
  uint16_t flow_id;
  uint16_t channel_id;
  uint16_t channel_seq;
  uint16_t frag_index;
  uint16_t frag_count;
  uint32_t msg_id;
  uint32_t replace_key;
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
  uint16_t next_send_seq;
  uint16_t expected_ordered;
  uint16_t latest_sequenced;
  uint8_t have_latest_sequenced;
} rudp_channel_state;

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
  uint32_t rx_seen[1024];
  uint8_t rx_seen_used[1024];
  uint64_t next_retx_ns;
  uint32_t inflight_bytes;
  uint32_t send_queue_bytes;
  uint32_t retransmit_queue_bytes;
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

static int ring_pop(rudp_ring* r, uint32_t* out) {
  if (r->count == 0) return 0;
  *out = r->items[r->head];
  r->head = (r->head + 1u) % r->cap;
  --r->count;
  return 1;
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
  if (seq > a->max_seq) {
    uint32_t shift = seq - a->max_seq;
    a->bits = (shift > 64u) ? 0 : ((a->bits << shift) | (1ull << (shift - 1u)));
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

static int addr_equal(const rudp_addr* a, const rudp_addr* b) {
  if (a->len > sizeof(a->data) || b->len > sizeof(b->data)) return 0;
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
  if (ep->now_ns != 0) return ep->now_ns;
  return ep->cfg.socket.now_ns ? ep->cfg.socket.now_ns(ep->cfg.socket.user) : 0;
}

static void free_msg(rudp_endpoint* ep, uint32_t idx) {
  if (idx >= ep->cfg.max_messages) return;
  rudp_msg* m = &ep->msgs[idx];
  memset(m, 0, sizeof(*m));
  m->data = ep->msg_data + ((size_t)idx * ep->frame_payload);
  ep->free_msgs[ep->free_msg_count++] = idx;
}

static uint32_t alloc_msg(rudp_endpoint* ep) {
  if (ep->free_msg_count == 0) return RUDP_INVALID_INDEX;
  uint32_t idx = ep->free_msgs[--ep->free_msg_count];
  rudp_msg* m = &ep->msgs[idx];
  memset(m, 0, sizeof(*m));
  m->used = 1;
  m->data = ep->msg_data + ((size_t)idx * ep->frame_payload);
  return idx;
}

static void enqueue_recv(rudp_conn* c, uint16_t flow_id, uint16_t channel_id,
                         rudp_reliability reliability, const uint8_t* data,
                         uint16_t len, int borrow) {
  rudp_endpoint* ep = c->ep;
  if (ep->recv_count >= ep->cfg.max_recv_events || len > ep->max_payload) return;
  rudp_recv_event_i* e = &ep->recv_events[ep->recv_tail];
  e->used = 1;
  e->borrowed = borrow ? 1u : 0u;
  e->conn_index = c->index;
  e->conn_id = c->conn_id;
  e->flow_id = flow_id;
  e->channel_id = channel_id;
  e->reliability = reliability;
  e->len = len;
  if (borrow) {
    e->data = (uint8_t*)data;
  } else {
    e->data = ep->recv_event_data + ((size_t)ep->recv_tail * ep->max_payload);
    if (len > 0) memcpy(e->data, data, len);
  }
  ep->recv_tail = (ep->recv_tail + 1u) % ep->cfg.max_recv_events;
  ++ep->recv_count;
}

static void release_sent_packet(rudp_conn* c, uint32_t seq) {
  rudp_endpoint* ep = c->ep;
  size_t slot = (size_t)c->index * ep->cfg.sent_packet_count +
                (seq % ep->cfg.sent_packet_count);
  rudp_sent_packet* sp = &ep->sent_packets[slot];
  if (!sp->used || sp->conn_index != c->index || sp->seq != seq) return;
  for (uint16_t r = 0; r < sp->ref_count; ++r) {
    uint32_t mi = sp->refs[r];
    if (mi >= ep->cfg.max_messages) continue;
    rudp_msg* m = &ep->msgs[mi];
    if (!m->used) continue;
    if (c->inflight_bytes >= m->len) c->inflight_bytes -= m->len;
    free_msg(ep, mi);
  }
  sp->used = 0;
}

static int ack_covers_seq(uint32_t ack, uint64_t bits, uint32_t seq) {
  if (seq == 0 || ack == 0) return 0;
  if (seq == ack) return 1;
  if (seq > ack) return 0;
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

static void track_reliable_seq(rudp_conn* c, uint32_t seq) {
  rudp_endpoint* ep = c->ep;
  uint32_t cap = ep->cfg.sent_packet_count;
  if (cap == 0 || !ep->sent_seq_queue) return;
  size_t base = (size_t)c->index * cap;
  if (c->sent_seq_count >= cap) {
    c->sent_seq_head = (c->sent_seq_head + 1u) % cap;
    --c->sent_seq_count;
  }
  ep->sent_seq_queue[base + c->sent_seq_tail] = seq;
  c->sent_seq_tail = (c->sent_seq_tail + 1u) % cap;
  ++c->sent_seq_count;
}

static void release_acked_packets(rudp_conn* c, uint32_t ack, uint64_t bits) {
  if (ack == 0 || c->sent_seq_count == 0) return;
  rudp_endpoint* ep = c->ep;
  uint32_t cap = ep->cfg.sent_packet_count;
  if (cap == 0 || !ep->sent_seq_queue) return;
  size_t base = (size_t)c->index * cap;
  uint32_t n = c->sent_seq_count;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t seq = ep->sent_seq_queue[base + c->sent_seq_head];
    c->sent_seq_head = (c->sent_seq_head + 1u) % cap;
    --c->sent_seq_count;
    rudp_sent_packet* sp = sent_packet_for_seq(c, seq);
    if (!sp) continue;
    if (ack_covers_seq(ack, bits, seq)) {
      release_sent_packet(c, seq);
      continue;
    }
    if (sp->lost) continue;
    ep->sent_seq_queue[base + c->sent_seq_tail] = seq;
    c->sent_seq_tail = (c->sent_seq_tail + 1u) % cap;
    ++c->sent_seq_count;
  }
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
  c->flows = ep->flow_storage + ((size_t)idx * ep->cfg.max_flows);
  c->channels = ep->channel_storage + ((size_t)idx * ep->cfg.max_channels);
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
  if (out) *out = c;
  return 0;
}

int rudp_endpoint_create(rudp_endpoint** out, const rudp_endpoint_config* config) {
  if (!out || !config || !config->socket.send_batch || !config->socket.recv_batch) return -1;
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
  ep->mtu = ep->cfg.mtu;
  if (ep->mtu <= RUDP_PACKET_HEADER_BYTES + RUDP_DATA_FRAME_HEADER_BYTES) goto fail;
  ep->frame_payload = (uint16_t)(ep->mtu - RUDP_PACKET_HEADER_BYTES -
                                 RUDP_DATA_FRAME_HEADER_BYTES);
  ep->max_payload = cfg_u16(ep->cfg.max_payload_bytes, ep->frame_payload);
  if (ep->max_payload == 0) goto fail;
  ep->per_conn_queue_cap = ep->cfg.max_messages / ep->cfg.max_conns;
  if (ep->per_conn_queue_cap < 1024u) ep->per_conn_queue_cap = 1024u;
  ep->ring_storage_stride = ep->per_conn_queue_cap + 1u;
  ep->conn_map_cap = next_pow2_u32(ep->cfg.max_conns * 4u);
  if (ep->conn_map_cap < 16u) ep->conn_map_cap = 16u;

  ep->conns = (rudp_conn*)calloc(ep->cfg.max_conns, sizeof(rudp_conn));
  ep->flow_storage = (rudp_flow_state*)calloc((size_t)ep->cfg.max_conns * ep->cfg.max_flows,
                                             sizeof(rudp_flow_state));
  ep->channel_storage = (rudp_channel_state*)calloc((size_t)ep->cfg.max_conns * ep->cfg.max_channels,
                                                   sizeof(rudp_channel_state));
  ep->ring_storage = (uint32_t*)calloc(
      (size_t)ep->cfg.max_conns * ep->cfg.max_flows * 3u *
          ep->ring_storage_stride,
      sizeof(uint32_t));
  ep->conn_map = (rudp_conn_map_entry*)calloc(ep->conn_map_cap,
                                              sizeof(rudp_conn_map_entry));
  ep->msgs = (rudp_msg*)calloc(ep->cfg.max_messages, sizeof(rudp_msg));
  ep->msg_data = (uint8_t*)calloc((size_t)ep->cfg.max_messages, ep->frame_payload);
  ep->free_msgs = (uint32_t*)calloc(ep->cfg.max_messages, sizeof(uint32_t));
  ep->sent_packets = (rudp_sent_packet*)calloc(
      (size_t)ep->cfg.max_conns * ep->cfg.sent_packet_count,
      sizeof(rudp_sent_packet));
  ep->sent_seq_queue = (uint32_t*)calloc(
      (size_t)ep->cfg.max_conns * ep->cfg.sent_packet_count,
      sizeof(uint32_t));
  ep->holds = (rudp_ordered_hold*)calloc(ep->cfg.max_ordered_holds, sizeof(rudp_ordered_hold));
  ep->hold_data = (uint8_t*)calloc((size_t)ep->cfg.max_ordered_holds, ep->max_payload);
  ep->recv_events = (rudp_recv_event_i*)calloc(ep->cfg.max_recv_events, sizeof(rudp_recv_event_i));
  ep->recv_event_data = (uint8_t*)calloc((size_t)ep->cfg.max_recv_events, ep->max_payload);
  ep->reasm_count = ep->cfg.max_recv_events;
  ep->reasm = (rudp_rx_reasm*)calloc(ep->reasm_count, sizeof(rudp_rx_reasm));
  ep->reasm_data = (uint8_t*)calloc((size_t)ep->reasm_count, ep->max_payload);
  ep->recv_batch = (rudp_in_packet*)calloc(ep->cfg.recv_batch_size, sizeof(rudp_in_packet));
  ep->recv_batch_data = (uint8_t*)calloc((size_t)ep->cfg.recv_batch_size, ep->mtu);
  ep->send_batch = (rudp_out_packet*)calloc(ep->cfg.send_batch_size, sizeof(rudp_out_packet));
  ep->send_batch_data = (uint8_t*)calloc((size_t)ep->cfg.send_batch_size, ep->mtu);
  ep->send_batch_conn = (uint32_t*)calloc(ep->cfg.send_batch_size, sizeof(uint32_t));
  ep->send_batch_refs = (uint32_t*)calloc((size_t)ep->cfg.send_batch_size * RUDP_MAX_REFS_PER_PACKET,
                                         sizeof(uint32_t));
  ep->send_batch_msgs = (uint32_t*)calloc((size_t)ep->cfg.send_batch_size * RUDP_MAX_REFS_PER_PACKET,
                                         sizeof(uint32_t));
  ep->send_batch_ref_counts = (uint16_t*)calloc(ep->cfg.send_batch_size, sizeof(uint16_t));
  ep->send_batch_msg_counts = (uint16_t*)calloc(ep->cfg.send_batch_size, sizeof(uint16_t));
  ep->send_batch_ack_only = (uint8_t*)calloc(ep->cfg.send_batch_size, sizeof(uint8_t));
  if (!ep->conns || !ep->flow_storage || !ep->channel_storage ||
      !ep->ring_storage || !ep->conn_map || !ep->msgs || !ep->msg_data ||
      !ep->free_msgs || !ep->sent_packets || !ep->sent_seq_queue ||
      !ep->holds || !ep->hold_data ||
      !ep->recv_events || !ep->recv_event_data || !ep->reasm || !ep->reasm_data ||
      !ep->recv_batch || !ep->recv_batch_data || !ep->send_batch ||
      !ep->send_batch_data || !ep->send_batch_conn || !ep->send_batch_refs ||
      !ep->send_batch_msgs || !ep->send_batch_ref_counts ||
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
  free(ep->reasm);
  free(ep->reasm_data);
  free(ep->recv_batch);
  free(ep->recv_batch_data);
  free(ep->send_batch);
  free(ep->send_batch_data);
  free(ep->send_batch_conn);
  free(ep->send_batch_refs);
  free(ep->send_batch_msgs);
  free(ep->send_batch_ref_counts);
  free(ep->send_batch_msg_counts);
  free(ep->send_batch_ack_only);
  free(ep);
}

int rudp_endpoint_connect(rudp_endpoint* ep, const rudp_addr* peer, uint64_t conn_id,
                          rudp_conn** out) {
  if (!ep || !peer || conn_id == 0) return -1;
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
  out->safe_bps = conn->ep->cfg.initial_safe_bps;
  out->pacing_bps = conn->ep->cfg.initial_safe_bps;
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
    if (f->limit.max_bps == UINT32_MAX) f->tokens_bytes = UINT32_MAX;
  }
}

static void refill_flow(rudp_flow_state* f, uint64_t now_ns) {
  if (f->limit.max_bps == UINT32_MAX) {
    f->tokens_bytes = UINT32_MAX;
    f->last_refill_ns = now_ns;
    return;
  }
  if (f->last_refill_ns == 0) {
    f->last_refill_ns = now_ns;
    f->tokens_bytes = f->limit.max_bps / 8u;
    return;
  }
  if (now_ns <= f->last_refill_ns) return;
  uint64_t add = ((uint64_t)f->limit.max_bps * (now_ns - f->last_refill_ns)) /
                 (8ull * RUDP_NS_PER_SEC);
  uint64_t cap = f->limit.max_bps / 8u;
  if (cap < 1500u) cap = 1500u;
  f->tokens_bytes = (f->tokens_bytes + add > cap) ? cap : f->tokens_bytes + add;
  f->last_refill_ns = now_ns;
}

rudp_send_result rudp_send(rudp_conn* conn, const void* data, size_t len,
                           const rudp_send_opts* opts) {
  if (!conn || !conn->active || !data) return RUDP_SEND_UNUSABLE;
  rudp_send_opts def;
  memset(&def, 0, sizeof(def));
  def.reliability = RUDP_UNRELIABLE;
  if (!opts) opts = &def;
  if (opts->flow_id >= conn->ep->cfg.max_flows ||
      opts->channel_id >= conn->ep->cfg.max_channels ||
      len > conn->ep->max_payload) {
    return RUDP_SEND_QUEUE_FULL;
  }
  if (len == 0) len = 0;
  rudp_flow_state* f = &conn->flows[opts->flow_id];
  if (f->limit.max_bps == 0) {
    ++f->stats.send_rate_limited;
    return RUDP_SEND_RATE_LIMITED;
  }
  if (f->limit.max_queue_bytes != UINT32_MAX &&
      f->queued_bytes + len > f->limit.max_queue_bytes) {
    ++f->stats.send_queue_full;
    return RUDP_SEND_QUEUE_FULL;
  }
  uint16_t frag_count = (uint16_t)((len + conn->ep->frame_payload - 1u) /
                                   conn->ep->frame_payload);
  if (frag_count == 0) frag_count = 1;
  if (frag_count > RUDP_MAX_FRAGS_PER_MSG || conn->ep->free_msg_count < frag_count) {
    ++f->stats.send_queue_full;
    return RUDP_SEND_QUEUE_FULL;
  }
  rudp_channel_state* ch = channel_for(conn, opts->channel_id);
  if (!ch) return RUDP_SEND_QUEUE_FULL;
  rudp_ring* q = (opts->reliability == RUDP_RELIABLE_UNORDERED ||
                  opts->reliability == RUDP_RELIABLE_ORDERED)
                     ? &f->reliable_q
                     : &f->unreliable_q;
  if (q->count + frag_count + 1u >= q->cap) {
    ++f->stats.send_queue_full;
    return RUDP_SEND_QUEUE_FULL;
  }
  uint32_t allocated[RUDP_MAX_FRAGS_PER_MSG];
  uint32_t msg_id = conn->next_msg_id++;
  uint16_t channel_seq = ch->next_send_seq++;
  uint64_t enqueue_ns = endpoint_now_ns(conn->ep);
  uint64_t deadline_ns =
      opts->deadline_ms ? enqueue_ns + (uint64_t)opts->deadline_ms * RUDP_NS_PER_MS : 0;
  if (f->limit.max_delay_ms > 0) {
    uint64_t flow_deadline = enqueue_ns + (uint64_t)f->limit.max_delay_ms * RUDP_NS_PER_MS;
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
    size_t offset = (size_t)frag * conn->ep->frame_payload;
    size_t remaining = (len > offset) ? len - offset : 0;
    size_t frag_len = remaining > conn->ep->frame_payload
                          ? conn->ep->frame_payload
                          : remaining;
    m->len = (uint16_t)frag_len;
    if (frag_len > 0) memcpy(m->data, (const uint8_t*)data + offset, frag_len);
  }
  for (uint16_t frag = 0; frag < frag_count; ++frag) {
    (void)ring_push(q, allocated[frag]);
  }
  f->queued_bytes += (uint32_t)len;
  f->stats.queued_bytes = f->queued_bytes;
  ++f->stats.send_ok;
  conn->send_queue_bytes += (uint32_t)len;
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

static int pick_from_flow(rudp_conn* c, rudp_flow_state* f, uint64_t now_ns, uint32_t* out) {
  uint32_t mi;
  while (ring_pop(&f->retry_q, &mi)) {
    rudp_msg* m = &c->ep->msgs[mi];
    if (!m->used) continue;
    m->in_retry = 0;
    if (c->retransmit_queue_bytes >= m->len) c->retransmit_queue_bytes -= m->len;
    *out = mi;
    return 1;
  }
  rudp_ring* qs[2] = {&f->reliable_q, &f->unreliable_q};
  for (uint32_t q = 0; q < 2; ++q) {
    while (ring_pop(qs[q], &mi)) {
      rudp_msg* m = &c->ep->msgs[mi];
      if (!m->used) continue;
      if (m->deadline_ns && now_ns >= m->deadline_ns) {
        ++f->stats.expired;
        ++f->stats.send_dropped;
        if (f->queued_bytes >= m->len) f->queued_bytes -= m->len;
        if (c->send_queue_bytes >= m->len) c->send_queue_bytes -= m->len;
        free_msg(c->ep, mi);
        continue;
      }
      if (f->limit.max_bps != UINT32_MAX && f->tokens_bytes < m->len) {
        (void)ring_push(qs[q], mi);
        return 0;
      }
      if (f->limit.max_bps != UINT32_MAX) f->tokens_bytes -= m->len;
      *out = mi;
      return 1;
    }
  }
  return 0;
}

static int pick_next_msg(rudp_conn* c, uint64_t now_ns, uint32_t* out) {
  for (uint32_t i = 0; i < c->ep->cfg.max_flows; ++i) {
    rudp_flow_state* f = &c->flows[i];
    if (f->retry_q.count > 0 && pick_from_flow(c, f, now_ns, out)) return 1;
  }
  for (uint32_t tries = 0; tries < c->ep->cfg.max_flows; ++tries) {
    uint32_t idx = (c->flow_cursor + tries) % c->ep->cfg.max_flows;
    rudp_flow_state* f = &c->flows[idx];
    refill_flow(f, now_ns);
    f->deficit += f->quantum;
    if (pick_from_flow(c, f, now_ns, out)) {
      c->flow_cursor = (idx + 1u) % c->ep->cfg.max_flows;
      return 1;
    }
  }
  return 0;
}

static void queue_retransmit(rudp_conn* c, uint32_t mi) {
  rudp_msg* m = &c->ep->msgs[mi];
  if (!m->used || m->in_retry) return;
  rudp_flow_state* f = flow_for(c, m->flow_id);
  if (!f) return;
  if (ring_push(&f->retry_q, mi)) {
    m->in_retry = 1;
    c->retransmit_queue_bytes += m->len;
    ++f->stats.retransmits;
  }
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
    uint64_t due = sp->sent_ns + rto_ns;
    if (now_ns < due) {
      if (next_retx_ns == 0 || due < next_retx_ns) next_retx_ns = due;
      continue;
    }
    sp->lost = 1;
    for (uint16_t r = 0; r < sp->ref_count; ++r) queue_retransmit(c, sp->refs[r]);
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
  uint64_t due = now_ns + (uint64_t)ep->cfg.rto_ms * RUDP_NS_PER_MS;
  if (c->next_retx_ns == 0 || due < c->next_retx_ns) c->next_retx_ns = due;
  for (uint16_t i = 0; i < refs; ++i) {
    uint32_t mi = ep->send_batch_refs[(size_t)batch_index * RUDP_MAX_REFS_PER_PACKET + i];
    sp->refs[i] = mi;
    if (mi < ep->cfg.max_messages) {
      rudp_msg* m = &ep->msgs[mi];
      m->inflight = 1;
      c->inflight_bytes += m->len;
      if (c->send_queue_bytes >= m->len) c->send_queue_bytes -= m->len;
      rudp_flow_state* f = flow_for(c, m->flow_id);
      if (f && f->queued_bytes >= m->len) {
        f->queued_bytes -= m->len;
        f->stats.queued_bytes = f->queued_bytes;
      }
    }
  }
}

static void flush_send_batch(rudp_endpoint* ep, uint64_t now_ns) {
  if (ep->send_count == 0) return;
  int sent = ep->cfg.socket.send_batch(ep->cfg.socket.user, ep->send_batch, ep->send_count);
  if (sent < 0) sent = 0;
  if ((uint32_t)sent > ep->send_count) sent = (int)ep->send_count;
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
        if (m->reliability == RUDP_RELIABLE_UNORDERED ||
            m->reliability == RUDP_RELIABLE_ORDERED) {
          continue;
        }
        rudp_flow_state* f = c ? flow_for(c, m->flow_id) : NULL;
        if (f && f->queued_bytes >= m->len) {
          f->queued_bytes -= m->len;
          f->stats.queued_bytes = f->queued_bytes;
        }
        if (c && c->send_queue_bytes >= m->len) c->send_queue_bytes -= m->len;
        free_msg(ep, mi);
      }
    }
  }
  ep->send_count = 0;
}

static int append_msg_to_batch_packet(rudp_endpoint* ep, uint32_t batch_index,
                                      uint32_t mi) {
  rudp_msg* m = &ep->msgs[mi];
  uint16_t frame_len = (uint16_t)(RUDP_DATA_FRAME_HEADER_BYTES + m->len);
  if (ep->send_batch[batch_index].len + frame_len > ep->mtu) return 0;
  if (ep->send_batch_msg_counts[batch_index] >= RUDP_MAX_REFS_PER_PACKET) return 0;
  uint8_t* pkt = ep->send_batch_data + ((size_t)batch_index * ep->mtu);
  size_t offset = ep->send_batch[batch_index].len;
  write_data_frame_header(pkt + offset, m);
  if (m->len > 0) memcpy(pkt + offset + RUDP_DATA_FRAME_HEADER_BYTES, m->data, m->len);
  ep->send_batch[batch_index].len += frame_len;
  uint16_t payload_len = (uint16_t)(ep->send_batch[batch_index].len - RUDP_PACKET_HEADER_BYTES);
  store_u16(pkt + 30, payload_len);
  uint16_t msg_slot = ep->send_batch_msg_counts[batch_index]++;
  ep->send_batch_msgs[(size_t)batch_index * RUDP_MAX_REFS_PER_PACKET + msg_slot] = mi;
  if (m->reliability == RUDP_RELIABLE_UNORDERED ||
      m->reliability == RUDP_RELIABLE_ORDERED) {
    uint16_t ref_slot = ep->send_batch_ref_counts[batch_index]++;
    ep->send_batch_refs[(size_t)batch_index * RUDP_MAX_REFS_PER_PACKET + ref_slot] = mi;
  }
  return 1;
}

static int try_append_to_previous_packet(rudp_conn* c, uint32_t mi) {
  rudp_endpoint* ep = c->ep;
  if (ep->send_count == 0) return 0;
  uint32_t idx = ep->send_count - 1u;
  if (ep->send_batch_ack_only[idx] || ep->send_batch_conn[idx] != c->index) return 0;
  return append_msg_to_batch_packet(ep, idx, mi);
}

static int append_packet_for_msg(rudp_conn* c, uint32_t mi, uint64_t now_ns) {
  rudp_endpoint* ep = c->ep;
  if (try_append_to_previous_packet(c, mi)) {
    rudp_msg* m = &ep->msgs[mi];
    rudp_flow_state* f = flow_for(c, m->flow_id);
    if (f) f->stats.sent_bps += m->len;
    return 1;
  }
  if (ep->send_count >= ep->cfg.send_batch_size) flush_send_batch(ep, now_ns);
  if (ep->send_count >= ep->cfg.send_batch_size) return 0;
  uint8_t* pkt = ep->send_batch_data + ((size_t)ep->send_count * ep->mtu);
  uint32_t seq = c->next_packet_seq++;
  write_packet_header(pkt, 0, c, seq, 0);
  ep->send_batch[ep->send_count].addr = c->peer;
  ep->send_batch[ep->send_count].data = pkt;
  ep->send_batch[ep->send_count].len = RUDP_PACKET_HEADER_BYTES;
  ep->send_batch_conn[ep->send_count] = c->index;
  ep->send_batch_ref_counts[ep->send_count] = 0;
  ep->send_batch_msg_counts[ep->send_count] = 0;
  ep->send_batch_ack_only[ep->send_count] = 0;
  if (!append_msg_to_batch_packet(ep, ep->send_count, mi)) return 0;
  rudp_msg* m = &ep->msgs[mi];
  rudp_flow_state* f = flow_for(c, m->flow_id);
  if (f) f->stats.sent_bps += m->len;
  ++ep->send_count;
  return 1;
}

static void append_ack_only(rudp_conn* c, uint64_t now_ns) {
  rudp_endpoint* ep = c->ep;
  if (!c->ack_dirty) return;
  if (ep->send_count >= ep->cfg.send_batch_size) flush_send_batch(ep, now_ns);
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
    uint32_t guard = ep->cfg.max_messages;
    int sent_data = 0;
    while (guard-- > 0 && pick_next_msg(c, now_ns, &mi)) {
      if (!append_packet_for_msg(c, mi, now_ns)) break;
      sent_data = 1;
    }
    if (!sent_data) append_ack_only(c, now_ns);
  }
  flush_send_batch(ep, now_ns);
}

static rudp_conn* find_or_create_incoming_conn(rudp_endpoint* ep, uint64_t conn_id,
                                               const rudp_addr* peer) {
  rudp_conn* c = rudp_endpoint_find_conn(ep, conn_id);
  if (c) {
    if (!addr_equal(&c->peer, peer)) c->peer = *peer;
    return c;
  }
  for (uint32_t i = 0; i < ep->cfg.max_conns; ++i) {
    if (!ep->conns[i].active) {
      if (create_conn_at(ep, i, peer, conn_id, &c) == 0) return c;
      return NULL;
    }
  }
  return NULL;
}

static int reliable_seen(rudp_conn* c, uint32_t msg_id) {
  uint32_t idx = msg_id & 1023u;
  if (c->rx_seen_used[idx] && c->rx_seen[idx] == msg_id) return 1;
  c->rx_seen_used[idx] = 1;
  c->rx_seen[idx] = msg_id;
  return 0;
}

static int hold_ordered(rudp_conn* c, uint16_t flow_id, uint16_t channel_id,
                        uint16_t channel_seq, rudp_reliability reliability,
                        const uint8_t* data, uint16_t len) {
  rudp_endpoint* ep = c->ep;
  for (uint32_t i = 0; i < ep->cfg.max_ordered_holds; ++i) {
    if (ep->holds[i].used) continue;
    rudp_ordered_hold* h = &ep->holds[i];
    h->used = 1;
    h->conn_index = c->index;
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
      enqueue_recv(c, h->flow_id, h->channel_id, h->reliability, h->data, h->len, 0);
      h->used = 0;
      ++ch->expected_ordered;
      progressed = 1;
      break;
    }
  }
}

static rudp_reliability reliability_from_flags(uint8_t flags) {
  if (flags & RUDP_F_RELIABLE) {
    return (flags & RUDP_F_ORDERED) ? RUDP_RELIABLE_ORDERED : RUDP_RELIABLE_UNORDERED;
  }
  if (flags & RUDP_F_SEQUENCED) return RUDP_UNRELIABLE_SEQUENCED;
  return RUDP_UNRELIABLE;
}

static void deliver_frame(rudp_conn* c, uint8_t flags, uint16_t flow_id,
                          uint16_t payload_len, uint32_t msg_id,
                          uint16_t channel_id, uint16_t channel_seq,
                          const uint8_t* payload, int borrow) {
  if (flags == 0) {
    if (channel_id >= c->ep->cfg.max_channels) return;
    enqueue_recv(c, flow_id, channel_id, RUDP_UNRELIABLE, payload, payload_len,
                 borrow);
    return;
  }
  rudp_reliability rel = reliability_from_flags(flags);
  rudp_channel_state* ch = channel_for(c, channel_id);
  if (!ch) return;
  if ((rel == RUDP_RELIABLE_UNORDERED || rel == RUDP_RELIABLE_ORDERED) &&
      reliable_seen(c, msg_id)) {
    return;
  }
  if (rel == RUDP_UNRELIABLE_SEQUENCED) {
    if (ch->have_latest_sequenced && channel_seq <= ch->latest_sequenced) return;
    ch->have_latest_sequenced = 1;
    ch->latest_sequenced = channel_seq;
    enqueue_recv(c, flow_id, channel_id, rel, payload, payload_len, borrow);
    return;
  }
  if (rel == RUDP_RELIABLE_ORDERED) {
    if (channel_seq < ch->expected_ordered) return;
    if (channel_seq > ch->expected_ordered) {
      (void)hold_ordered(c, flow_id, channel_id, channel_seq, rel, payload, payload_len);
      return;
    }
    enqueue_recv(c, flow_id, channel_id, rel, payload, payload_len, borrow);
    ++ch->expected_ordered;
    drain_ordered(c, channel_id);
    return;
  }
  enqueue_recv(c, flow_id, channel_id, rel, payload, payload_len, borrow);
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
                                          rudp_reliability reliability) {
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
  free_slot->flow_id = flow_id;
  free_slot->channel_id = channel_id;
  free_slot->channel_seq = channel_seq;
  free_slot->frag_count = frag_count;
  free_slot->reliability = reliability;
  free_slot->data = ep->reasm_data + ((size_t)idx * ep->max_payload);
  return free_slot;
}

static void process_data_frame(rudp_conn* c, uint8_t flags, uint16_t flow_id,
                               uint16_t payload_len, uint32_t msg_id,
                               uint16_t frag_index, uint16_t frag_count,
                               uint16_t channel_id, uint16_t channel_seq,
                               const uint8_t* payload) {
  if (frag_count == 0 || frag_index >= frag_count ||
      frag_count > RUDP_MAX_FRAGS_PER_MSG) {
    return;
  }
  if (frag_count == 1) {
    deliver_frame(c, flags, flow_id, payload_len, msg_id, channel_id,
                  channel_seq, payload, 1);
    return;
  }
  size_t offset = (size_t)frag_index * c->ep->frame_payload;
  if (offset + payload_len > c->ep->max_payload) return;
  rudp_reliability rel = reliability_from_flags(flags);
  rudp_rx_reasm* r = find_or_alloc_reasm(c, msg_id, flow_id, channel_id,
                                         channel_seq, frag_count, rel);
  if (!r || r->frag_count != frag_count || r->flow_id != flow_id ||
      r->channel_id != channel_id || r->channel_seq != channel_seq) {
    return;
  }
  if (reasm_has_frag(r, frag_index)) return;
  if (payload_len > 0) memcpy(r->data + offset, payload, payload_len);
  reasm_set_frag(r, frag_index);
  ++r->received_count;
  size_t end = offset + payload_len;
  if (end > r->total_len) r->total_len = (uint16_t)end;
  if (r->received_count == r->frag_count) {
    deliver_frame(c, flags, r->flow_id, r->total_len, r->msg_id,
                  r->channel_id, r->channel_seq, r->data, 0);
    r->used = 0;
  }
}

static void process_packet(rudp_endpoint* ep, const rudp_in_packet* in, uint64_t now_ns) {
  if (in->len < RUDP_PACKET_HEADER_BYTES) return;
  const uint8_t* p = in->data;
  uint32_t magic_flags = load_u32(p + 0);
  if ((magic_flags & RUDP_PACKET_MAGIC_MASK) != RUDP_PACKET_MAGIC) return;
  uint8_t packet_flags = (uint8_t)(magic_flags & 0xffu);
  uint64_t conn_id = load_u64(p + 4);
  uint32_t packet_seq = load_u32(p + 12);
  uint32_t ack_seq = load_u32(p + 16);
  uint64_t ack_bits = load_u64(p + 20);
  uint16_t header_len = load_u16(p + 28);
  uint16_t payload_len = load_u16(p + 30);
  if (header_len != RUDP_PACKET_HEADER_BYTES || header_len + payload_len > in->len) return;
  rudp_conn* c = find_or_create_incoming_conn(ep, conn_id, &in->addr);
  if (!c) return;
  if (ack_seq != 0) release_acked_packets(c, ack_seq, ack_bits);
  if ((packet_flags & RUDP_PACKET_FLAG_ACK_ONLY) != 0) return;

  size_t off = header_len;
  size_t end = header_len + payload_len;
  int packet_has_reliable = 0;
  while (off + RUDP_DATA_FRAME_HEADER_BYTES <= end) {
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
    process_data_frame(c, flags, flow_id, frame_payload_len, msg_id, frag_index,
                       frag_count, channel_id, channel_seq,
                       p + off + frame_header_len);
    (void)now_ns;
    off += frame_header_len + frame_payload_len;
  }
  if (packet_seq != 0 &&
      (!ep->cfg.skip_unreliable_acks || packet_has_reliable) &&
      ack_insert(&c->ack, packet_seq)) {
    c->ack_dirty = 1;
  }
}

void rudp_endpoint_poll(rudp_endpoint* ep, uint64_t now_ns) {
  if (!ep) return;
  ep->now_ns = now_ns;
  int n = ep->cfg.socket.recv_batch(ep->cfg.socket.user, ep->recv_batch,
                                    ep->cfg.recv_batch_size);
  if (n <= 0) return;
  if ((uint32_t)n > ep->cfg.recv_batch_size) n = (int)ep->cfg.recv_batch_size;
  for (int i = 0; i < n; ++i) process_packet(ep, &ep->recv_batch[i], now_ns);
}

int rudp_recv_borrow(rudp_endpoint* ep, const void** data, size_t* out_len,
                     rudp_recv_info* out_info) {
  if (!ep || !data || !out_len || ep->recv_count == 0) return 0;
  rudp_recv_event_i* e = &ep->recv_events[ep->recv_head];
  if (!e->used) return 0;
  *data = e->data;
  *out_len = e->len;
  if (out_info) {
    out_info->conn = conn_by_index(ep, e->conn_index);
    out_info->conn_id = e->conn_id;
    out_info->flow_id = e->flow_id;
    out_info->channel_id = e->channel_id;
    out_info->reliability = e->reliability;
  }
  e->used = 0;
  ep->recv_head = (ep->recv_head + 1u) % ep->cfg.max_recv_events;
  --ep->recv_count;
  return 1;
}

int rudp_recv_borrow_meta(rudp_endpoint* ep, const void** data, size_t* out_len,
                          uint64_t* out_conn_id,
                          rudp_reliability* out_reliability) {
  if (!ep || !data || !out_len || ep->recv_count == 0) return 0;
  rudp_recv_event_i* e = &ep->recv_events[ep->recv_head];
  if (!e->used) return 0;
  *data = e->data;
  *out_len = e->len;
  if (out_conn_id) *out_conn_id = e->conn_id;
  if (out_reliability) *out_reliability = e->reliability;
  e->used = 0;
  ep->recv_head = (ep->recv_head + 1u) % ep->cfg.max_recv_events;
  --ep->recv_count;
  return 1;
}

int rudp_recv(rudp_endpoint* ep, void* data, size_t cap, size_t* out_len,
              rudp_recv_info* out_info) {
  if (!ep || !data || !out_len || ep->recv_count == 0) return 0;
  rudp_recv_event_i* e = &ep->recv_events[ep->recv_head];
  if (!e->used) return 0;
  if (e->len > cap) return -1;
  if (e->len > 0) memcpy(data, e->data, e->len);
  *out_len = e->len;
  if (out_info) {
    out_info->conn = conn_by_index(ep, e->conn_index);
    out_info->conn_id = e->conn_id;
    out_info->flow_id = e->flow_id;
    out_info->channel_id = e->channel_id;
    out_info->reliability = e->reliability;
  }
  e->used = 0;
  ep->recv_head = (ep->recv_head + 1u) % ep->cfg.max_recv_events;
  --ep->recv_count;
  return 1;
}
