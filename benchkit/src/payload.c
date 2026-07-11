#include "benchkit.h"

#include <stddef.h>
#include <stdint.h>

static void write_u64_le(uint8_t *p, uint64_t v) {
  for (size_t i = 0; i < 8; ++i) {
    p[i] = (uint8_t)((v >> (i * 8u)) & 0xffu);
  }
}

static void write_u32_le(uint8_t *p, uint32_t v) {
  for (size_t i = 0; i < 4; ++i) {
    p[i] = (uint8_t)((v >> (i * 8u)) & 0xffu);
  }
}

static uint64_t read_u64_le(const uint8_t *p) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) {
    v |= (uint64_t)p[i] << (i * 8u);
  }
  return v;
}

static uint32_t read_u32_le(const uint8_t *p) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) {
    v |= (uint32_t)p[i] << (i * 8u);
  }
  return v;
}

int bk_payload_write(void *buf, size_t len, const bk_header *h) {
  if (buf == NULL || h == NULL || len < BK_HEADER_SIZE) {
    return -1;
  }
  if ((h->flags & 0xe0u) != 0 ||
      BK_FLAGS_DIRECTION(h->flags) > BK_DIRECTION_SERVER_TO_CLIENT) {
    return -1;
  }

  uint8_t *p = (uint8_t *)buf;
  write_u64_le(p + 0, h->seq);
  write_u64_le(p + 8, h->sched_ts_ns);
  write_u64_le(p + 16, h->send_ts_ns);
  p[24] = h->flags;
  write_u32_le(p + 25, h->origin_id);
  p[29] = h->traffic_id;
  p[30] = 0;
  p[31] = 0;
  return 0;
}

int bk_payload_read(const void *buf, size_t len, bk_header *h) {
  if (buf == NULL || h == NULL || len < BK_HEADER_SIZE) {
    return -1;
  }

  const uint8_t *p = (const uint8_t *)buf;
  const uint8_t flags = p[24];
  if ((flags & 0xe0u) != 0 ||
      BK_FLAGS_DIRECTION(flags) > BK_DIRECTION_SERVER_TO_CLIENT || p[30] != 0 ||
      p[31] != 0) {
    return -1;
  }

  h->seq = read_u64_le(p + 0);
  h->sched_ts_ns = read_u64_le(p + 8);
  h->send_ts_ns = read_u64_le(p + 16);
  h->flags = flags;
  h->origin_id = read_u32_le(p + 25);
  h->traffic_id = p[29];
  return 0;
}

static uint64_t splitmix64(uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30u)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27u)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31u);
}

static uint64_t body_pattern_key(const bk_header *h) {
  // prng-v1 folds the complete 32-byte wire header as four LE words.
  const uint64_t tail = (uint64_t)h->flags | ((uint64_t)h->origin_id << 8u) |
                        ((uint64_t)h->traffic_id << 40u);
  uint64_t key = UINT64_C(0x727564702d707231);  // "rudp-pr1"
  key = splitmix64(key ^ h->seq);
  key = splitmix64(key ^ h->sched_ts_ns);
  key = splitmix64(key ^ h->send_ts_ns);
  return splitmix64(key ^ tail);
}

static uint64_t body_pattern_block(uint64_t key, size_t block_index) {
  return splitmix64(key + (uint64_t)block_index *
                              UINT64_C(0x9e3779b97f4a7c15));
}

int bk_payload_fill_body(void *buf, size_t len, const bk_header *h) {
  if (buf == NULL || h == NULL || len < BK_HEADER_SIZE) {
    return -1;
  }
  uint8_t *p = (uint8_t *)buf;
  const uint64_t key = body_pattern_key(h);
  size_t body_index = 0;
  while (BK_HEADER_SIZE + body_index < len) {
    const uint64_t block = body_pattern_block(key, body_index / 8u);
    for (size_t offset = 0;
         offset < 8u && BK_HEADER_SIZE + body_index < len;
         ++offset, ++body_index) {
      p[BK_HEADER_SIZE + body_index] =
          (uint8_t)(block >> (unsigned)(offset * 8u));
    }
  }
  return 0;
}

int bk_payload_validate_body(const void *buf, size_t len,
                             const bk_header *h) {
  if (buf == NULL || h == NULL || len < BK_HEADER_SIZE) {
    return -1;
  }
  const uint8_t *p = (const uint8_t *)buf;
  const uint64_t key = body_pattern_key(h);
  size_t body_index = 0;
  while (BK_HEADER_SIZE + body_index < len) {
    const uint64_t block = body_pattern_block(key, body_index / 8u);
    for (size_t offset = 0;
         offset < 8u && BK_HEADER_SIZE + body_index < len;
         ++offset, ++body_index) {
      if (p[BK_HEADER_SIZE + body_index] !=
          (uint8_t)(block >> (unsigned)(offset * 8u))) {
        return -1;
      }
    }
  }
  return 0;
}

int bk_authoritative_state_write_applied_input_seq(void *buf, size_t len,
                                                   uint64_t seq) {
  if (buf == NULL || len < BK_AUTHORITATIVE_STATE_MIN_PAYLOAD) {
    return -1;
  }
  write_u64_le((uint8_t *)buf + BK_HEADER_SIZE, seq);
  return 0;
}

int bk_authoritative_state_read_applied_input_seq(const void *buf, size_t len,
                                                  uint64_t *out) {
  if (buf == NULL || out == NULL || len < BK_AUTHORITATIVE_STATE_MIN_PAYLOAD) {
    return -1;
  }
  *out = read_u64_le((const uint8_t *)buf + BK_HEADER_SIZE);
  return 0;
}

int bk_authoritative_state_fill_target_pad(void *buf, size_t len,
                                           uint32_t target_id) {
  if (buf == NULL || len < BK_AUTHORITATIVE_STATE_MIN_PAYLOAD) {
    return -1;
  }
  uint8_t *p = (uint8_t *)buf;
  for (size_t i = BK_AUTHORITATIVE_STATE_MIN_PAYLOAD; i < len; ++i) {
    const size_t pad_index = i - BK_AUTHORITATIVE_STATE_MIN_PAYLOAD;
    const unsigned shift = (unsigned)(pad_index % 4u) * 8u;
    const uint8_t target_byte = (uint8_t)(target_id >> shift);
    p[i] = (uint8_t)(target_byte ^ (uint8_t)(pad_index / 4u));
  }
  return 0;
}

int bk_authoritative_state_validate_target_pad(const void *buf, size_t len,
                                               uint32_t target_id) {
  if (buf == NULL || len < BK_AUTHORITATIVE_STATE_MIN_PAYLOAD) {
    return -1;
  }
  const uint8_t *p = (const uint8_t *)buf;
  for (size_t i = BK_AUTHORITATIVE_STATE_MIN_PAYLOAD; i < len; ++i) {
    const size_t pad_index = i - BK_AUTHORITATIVE_STATE_MIN_PAYLOAD;
    const unsigned shift = (unsigned)(pad_index % 4u) * 8u;
    const uint8_t target_byte = (uint8_t)(target_id >> shift);
    const uint8_t expected =
        (uint8_t)(target_byte ^ (uint8_t)(pad_index / 4u));
    if (p[i] != expected) {
      return -1;
    }
  }
  return 0;
}
