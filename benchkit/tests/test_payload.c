#include "benchkit.h"

#include "test_common.h"

#include <stdint.h>
#include <string.h>

int main(void) {
  uint8_t buf[40];
  memset(buf, 0xaa, sizeof(buf));

  const bk_header in = {
      .seq = 0x0102030405060708ull,
      .sched_ts_ns = 0x1112131415161718ull,
      .send_ts_ns = 0x2122232425262728ull,
      .flags = BK_FLAG_MUST_DELIVER | BK_FLAG_MEASURE | BK_FLAG_BROADCAST,
      .origin_id = 0xa1b2c3d4u,
  };

  CHECK(bk_payload_write(buf, sizeof(buf), &in) == 0);
  CHECK(buf[0] == 0x08);
  CHECK(buf[7] == 0x01);
  CHECK(buf[24] == in.flags);
  CHECK(buf[25] == 0xd4);
  CHECK(buf[28] == 0xa1);
  CHECK(buf[29] == 0);
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

  CHECK(bk_payload_write(buf, BK_HEADER_SIZE - 1u, &in) == -1);
  CHECK(bk_payload_read(buf, BK_HEADER_SIZE - 1u, &out) == -1);

  CHECK(bk_payload_write(buf, sizeof(buf), &in) == 0);
  buf[24] |= 0x08u;
  CHECK(bk_payload_read(buf, sizeof(buf), &out) == -1);

  CHECK(bk_payload_write(buf, sizeof(buf), &in) == 0);
  buf[29] = 1;
  CHECK(bk_payload_read(buf, sizeof(buf), &out) == -1);

  return 0;
}
