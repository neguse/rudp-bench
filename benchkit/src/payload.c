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

static uint8_t body_pattern_byte(const bk_header *h, size_t body_index) {
  const unsigned seq_shift = (unsigned)(body_index & 7u) * 8u;
  const unsigned sched_shift = (unsigned)((body_index + 3u) & 7u) * 8u;
  const unsigned send_shift = (unsigned)((body_index + 5u) & 7u) * 8u;
  const unsigned origin_shift = (unsigned)(body_index & 3u) * 8u;
  return (uint8_t)(
      (uint8_t)(h->seq >> seq_shift) ^
      (uint8_t)(h->sched_ts_ns >> sched_shift) ^
      (uint8_t)(h->send_ts_ns >> send_shift) ^
      (uint8_t)(h->origin_id >> origin_shift) ^ h->flags ^ h->traffic_id ^
      (uint8_t)body_index);
}

int bk_payload_fill_body(void *buf, size_t len, const bk_header *h) {
  if (buf == NULL || h == NULL || len < BK_HEADER_SIZE) {
    return -1;
  }
  uint8_t *p = (uint8_t *)buf;
  for (size_t i = BK_HEADER_SIZE; i < len; ++i) {
    p[i] = body_pattern_byte(h, i - BK_HEADER_SIZE);
  }
  return 0;
}

int bk_payload_validate_body(const void *buf, size_t len,
                             const bk_header *h) {
  if (buf == NULL || h == NULL || len < BK_HEADER_SIZE) {
    return -1;
  }
  const uint8_t *p = (const uint8_t *)buf;
  for (size_t i = BK_HEADER_SIZE; i < len; ++i) {
    if (p[i] != body_pattern_byte(h, i - BK_HEADER_SIZE)) {
      return -1;
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
