#include <gtest/gtest.h>
#include "harness/scenario.h"

TEST(Scenario, ParsesAllFlags) {
  const char* argv[] = {
      "rudp-bench",
      "--library=raw_udp", "--role=client",
      "--host=127.0.0.1", "--port=9000",
      "--rate-r=10", "--rate-u=100",
      "--size=64", "--conns=4",
      "--duration=30", "--warmup=2", "--loss=0",
      "--idle=adaptive", "--out=/tmp/out.csv",
  };
  int argc = sizeof(argv) / sizeof(argv[0]);
  auto cfg = rudp_bench::parse_scenario(argc, argv);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->library, "raw_udp");
  EXPECT_EQ(cfg->role, rudp_bench::Role::Client);
  EXPECT_EQ(cfg->host, "127.0.0.1");
  EXPECT_EQ(cfg->port, 9000);
  EXPECT_EQ(cfg->rate_r, 10u);
  EXPECT_EQ(cfg->rate_u, 100u);
  EXPECT_EQ(cfg->size_bytes, 64u);
  EXPECT_EQ(cfg->conns, 4u);
  EXPECT_EQ(cfg->duration_s, 30u);
  EXPECT_EQ(cfg->warmup_s, 2u);
  EXPECT_DOUBLE_EQ(cfg->loss_pct, 0.0);
  EXPECT_EQ(cfg->idle_policy, rudp_bench::IdlePolicy::Adaptive);
  EXPECT_STREQ(rudp_bench::idle_policy_name(cfg->idle_policy), "adaptive");
  EXPECT_EQ(cfg->out_path, "/tmp/out.csv");
}

TEST(Scenario, RejectsUnknownFlag) {
  const char* argv[] = {"rudp-bench", "--bogus=1"};
  EXPECT_FALSE(rudp_bench::parse_scenario(2, argv).has_value());
}

TEST(Scenario, RejectsBothRatesZero) {
  const char* argv[] = {
      "rudp-bench", "--library=raw_udp", "--role=client",
      "--rate-r=0", "--rate-u=0",
  };
  int argc = sizeof(argv) / sizeof(argv[0]);
  EXPECT_FALSE(rudp_bench::parse_scenario(argc, argv).has_value());
}

TEST(Scenario, RoleServerStillRequiresAtLeastOneRate) {
  const char* argv[] = {
      "rudp-bench", "--library=raw_udp", "--role=server",
      "--port=9000", "--rate-u=100", "--duration=30", "--out=/tmp/s.csv",
  };
  int argc = sizeof(argv) / sizeof(argv[0]);
  auto cfg = rudp_bench::parse_scenario(argc, argv);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->role, rudp_bench::Role::Server);
  EXPECT_EQ(cfg->rate_u, 100u);
}
