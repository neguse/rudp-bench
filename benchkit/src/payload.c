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

  uint8_t *p = (uint8_t *)buf;
  write_u64_le(p + 0, h->seq);
  write_u64_le(p + 8, h->sched_ts_ns);
  write_u64_le(p + 16, h->send_ts_ns);
  p[24] = h->flags;
  write_u32_le(p + 25, h->origin_id);
  p[29] = 0;
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
  if ((flags & 0xf8u) != 0 || p[29] != 0 || p[30] != 0 || p[31] != 0) {
    return -1;
  }

  h->seq = read_u64_le(p + 0);
  h->sched_ts_ns = read_u64_le(p + 8);
  h->send_ts_ns = read_u64_le(p + 16);
  h->flags = flags;
  h->origin_id = read_u32_le(p + 25);
  return 0;
}
