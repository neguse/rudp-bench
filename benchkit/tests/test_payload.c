#include "benchkit.h"

#include "test_common.h"

#include <stdint.h>
#include <string.h>

int main(void) {
  uint8_t buf[48];
  memset(buf, 0xaa, sizeof(buf));

  const bk_header in = {
      .seq = 0x0102030405060708ull,
      .sched_ts_ns = 0x1112131415161718ull,
      .send_ts_ns = 0x2122232425262728ull,
      .flags = BK_FLAG_MUST_DELIVER | BK_FLAG_MEASURE | BK_FLAG_BROADCAST |
               BK_FLAG_DIRECTION(BK_DIRECTION_SERVER_TO_CLIENT),
      .origin_id = 0xa1b2c3d4u,
      .traffic_id = 7,
  };

  CHECK(bk_payload_write(buf, sizeof(buf), &in) == 0);
  CHECK(buf[0] == 0x08);
  CHECK(buf[7] == 0x01);
  CHECK(buf[24] == in.flags);
  CHECK(buf[25] == 0xd4);
  CHECK(buf[28] == 0xa1);
  CHECK(buf[29] == in.traffic_id);
  CHECK(buf[30] == 0);
  CHECK(buf[31] == 0);
  CHECK(buf[32] == 0xaa);

  bk_header out;
  memset(&out, 0, sizeof(out));
  CHECK(bk_payload_read(buf, sizeof(buf), &out) == 0);
  CHECK(out.seq == in.seq);
  CHECK(out.sched_ts_ns == in.sched_ts_ns);
  CHECK(out.send_ts_ns == in.send_ts_ns);
  CHECK(out.flags == in.flags);
  CHECK(out.origin_id == in.origin_id);
  CHECK(out.traffic_id == in.traffic_id);
  CHECK(BK_FLAGS_DIRECTION(out.flags) == BK_DIRECTION_SERVER_TO_CLIENT);

  CHECK(bk_payload_fill_body(buf, sizeof(buf), &in) == 0);
  const uint8_t expected_body[] = {
      0x51, 0x78, 0xa7, 0xf1, 0x04, 0x73, 0x94, 0x5c,
      0x08, 0x30, 0x24, 0x40, 0xcc, 0x32, 0x41, 0x3e,
  };
  CHECK(memcmp(buf + BK_HEADER_SIZE, expected_body, sizeof(expected_body)) ==
        0);
  CHECK(bk_payload_validate_body(buf, sizeof(buf), &in) == 0);
  buf[47] ^= 1u;
  CHECK(bk_payload_validate_body(buf, sizeof(buf), &in) == -1);
  CHECK(bk_payload_fill_body(buf, BK_HEADER_SIZE, &in) == 0);
  CHECK(bk_payload_validate_body(buf, BK_HEADER_SIZE, &in) == 0);
  CHECK(bk_payload_fill_body(buf, BK_HEADER_SIZE - 1u, &in) == -1);
  CHECK(bk_payload_validate_body(buf, BK_HEADER_SIZE - 1u, &in) == -1);

  uint8_t representative[1000];
  CHECK(bk_payload_write(representative, sizeof(representative), &in) == 0);
  CHECK(bk_payload_fill_body(representative, sizeof(representative), &in) ==
        0);
  CHECK(memcmp(representative + BK_HEADER_SIZE,
               representative + BK_HEADER_SIZE + 256u, 256u) != 0);
  CHECK(memcmp(representative + BK_HEADER_SIZE + 256u,
               representative + BK_HEADER_SIZE + 512u, 256u) != 0);
  CHECK(representative[BK_HEADER_SIZE + 223u] == 0xdb);
  CHECK(representative[BK_HEADER_SIZE + 224u] == 0x90);
  CHECK(representative[BK_HEADER_SIZE + 255u] == 0x96);
  CHECK(representative[BK_HEADER_SIZE + 256u] == 0xd8);
  CHECK(representative[999] == 0x1c);
  bool seen[256] = {false};
  size_t distinct = 0;
  for (size_t i = BK_HEADER_SIZE; i < sizeof(representative); ++i) {
    if (!seen[representative[i]]) {
      seen[representative[i]] = true;
      ++distinct;
    }
  }
  CHECK(distinct >= 240u);

  bk_header changed = in;
  changed.seq ^= 1u;
  CHECK(bk_payload_fill_body(representative, sizeof(representative),
                             &changed) == 0);
  CHECK(memcmp(representative + BK_HEADER_SIZE, expected_body, 8u) != 0);
  changed = in;
  changed.sched_ts_ns ^= 1u;
  CHECK(bk_payload_fill_body(representative, sizeof(representative),
                             &changed) == 0);
  CHECK(memcmp(representative + BK_HEADER_SIZE, expected_body, 8u) != 0);
  changed = in;
  changed.send_ts_ns ^= 1u;
  CHECK(bk_payload_fill_body(representative, sizeof(representative),
                             &changed) == 0);
  CHECK(memcmp(representative + BK_HEADER_SIZE, expected_body, 8u) != 0);
  changed = in;
  changed.origin_id ^= 1u;
  CHECK(bk_payload_fill_body(representative, sizeof(representative),
                             &changed) == 0);
  CHECK(memcmp(representative + BK_HEADER_SIZE, expected_body, 8u) != 0);
  changed = in;
  changed.flags ^= BK_FLAG_MEASURE;
  CHECK(bk_payload_fill_body(representative, sizeof(representative),
                             &changed) == 0);
  CHECK(memcmp(representative + BK_HEADER_SIZE, expected_body, 8u) != 0);
  changed = in;
  changed.traffic_id ^= 1u;
  CHECK(bk_payload_fill_body(representative, sizeof(representative),
                             &changed) == 0);
  CHECK(memcmp(representative + BK_HEADER_SIZE, expected_body, 8u) != 0);

  CHECK(bk_payload_write(buf, BK_HEADER_SIZE - 1u, &in) == -1);
  CHECK(bk_payload_read(buf, BK_HEADER_SIZE - 1u, &out) == -1);

  CHECK(bk_payload_write(buf, sizeof(buf), &in) == 0);
  buf[24] = (uint8_t)((buf[24] & ~BK_FLAG_DIRECTION_MASK) |
                      BK_FLAG_DIRECTION_MASK);
  CHECK(bk_payload_read(buf, sizeof(buf), &out) == -1);

  CHECK(bk_payload_write(buf, sizeof(buf), &in) == 0);
  buf[30] = 1;
  CHECK(bk_payload_read(buf, sizeof(buf), &out) == -1);

  const uint64_t applied = 0x3132333435363738ull;
  CHECK(bk_authoritative_state_write_applied_input_seq(buf, sizeof(buf),
                                                       applied) == 0);
  CHECK(buf[32] == 0x38);
  CHECK(buf[39] == 0x31);
  uint64_t applied_out = 0;
  CHECK(bk_authoritative_state_read_applied_input_seq(
            buf, sizeof(buf), &applied_out) == 0);
  CHECK(applied_out == applied);
  CHECK(bk_authoritative_state_write_applied_input_seq(
            buf, BK_AUTHORITATIVE_STATE_MIN_PAYLOAD - 1u, applied) == -1);
  CHECK(bk_authoritative_state_read_applied_input_seq(
            buf, BK_AUTHORITATIVE_STATE_MIN_PAYLOAD - 1u, &applied_out) == -1);

  CHECK(bk_authoritative_state_fill_target_pad(buf, sizeof(buf),
                                               0x11223344u) == 0);
  CHECK(buf[40] == 0x44 && buf[41] == 0x33 && buf[42] == 0x22 &&
        buf[43] == 0x11);
  CHECK(buf[44] == (uint8_t)(0x44u ^ 1u));
  CHECK(bk_authoritative_state_validate_target_pad(
            buf, sizeof(buf), 0x11223344u) == 0);
  buf[47] ^= 1u;
  CHECK(bk_authoritative_state_validate_target_pad(
            buf, sizeof(buf), 0x11223344u) == -1);
  CHECK(bk_authoritative_state_fill_target_pad(buf, sizeof(buf),
                                               0x11223344u) == 0);
  CHECK(bk_authoritative_state_read_applied_input_seq(
            buf, sizeof(buf), &applied_out) == 0);
  CHECK(applied_out == applied);
  CHECK(bk_authoritative_state_fill_target_pad(
            buf, BK_AUTHORITATIVE_STATE_MIN_PAYLOAD - 1u, 0) == -1);
  CHECK(bk_authoritative_state_validate_target_pad(
            buf, BK_AUTHORITATIVE_STATE_MIN_PAYLOAD - 1u, 0) == -1);

  // v2 room-relay payload は v3 でも同じ 32-byte wire image と意味になる。
  const bk_header legacy = {
      .seq = 1,
      .sched_ts_ns = 2,
      .send_ts_ns = 3,
      .flags = 0,
      .origin_id = 4,
  };
  CHECK(bk_payload_write(buf, BK_HEADER_SIZE, &legacy) == 0);
  CHECK(buf[24] == 0 && buf[29] == 0 && buf[30] == 0 && buf[31] == 0);
  CHECK(bk_payload_read(buf, BK_HEADER_SIZE, &out) == 0);
  CHECK(out.traffic_id == BK_TRAFFIC_ID_ROOM_RELAY);
  CHECK(BK_FLAGS_DIRECTION(out.flags) == BK_DIRECTION_ROOM_RELAY);

  bk_header invalid = legacy;
  invalid.flags = 0x80u;
  CHECK(bk_payload_write(buf, BK_HEADER_SIZE, &invalid) == -1);
  invalid.flags = BK_FLAG_DIRECTION((bk_direction)3);
  CHECK(bk_payload_write(buf, BK_HEADER_SIZE, &invalid) == -1);

  return 0;
}
