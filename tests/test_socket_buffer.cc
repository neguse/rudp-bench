#include "harness/socket_buffer.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <string>

using namespace rudp_bench;

TEST(SocketBuffer, TunesAndReportsActualValues) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);

  testing::internal::CaptureStderr();
  SocketBufferResult r =
      tune_udp_socket_buffers(fd, 64 * 1024, 64 * 1024, "test", "unit");
  std::string err = testing::internal::GetCapturedStderr();

  EXPECT_TRUE(r.rcv_set_ok);
  EXPECT_TRUE(r.snd_set_ok);
  EXPECT_TRUE(r.rcv_get_ok);
  EXPECT_TRUE(r.snd_get_ok);
  EXPECT_GT(r.actual_rcv, 0);
  EXPECT_GT(r.actual_snd, 0);
  EXPECT_GT(r.effective_rcv, 0);
  EXPECT_GT(r.effective_snd, 0);
  EXPECT_NE(err.find("socket_buffer adapter=test socket=unit"), std::string::npos);
  EXPECT_NE(err.find("opt=SO_RCVBUF"), std::string::npos);
  EXPECT_NE(err.find("opt=SO_SNDBUF"), std::string::npos);
  EXPECT_NE(err.find("clamped="), std::string::npos);

  ::close(fd);
}
