#include <gtest/gtest.h>

#include <initializer_list>
#include <vector>

#include "harness/scenario.h"

namespace {

bool Parses(std::initializer_list<const char*> args) {
  std::vector<const char*> argv(args);
  return rudp_bench::parse_scenario(static_cast<int>(argv.size()), argv.data())
      .has_value();
}

}  // namespace

TEST(Scenario, ParsesAllFlags) {
  const char* argv[] = {
      "rudp-bench",
      "--library=raw_udp", "--role=client",
      "--host=127.0.0.1", "--port=9000",
      "--rate-r=10", "--rate-u=100",
      "--size=64", "--conns=4",
      "--fanout-conns=16", "--conn-id-offset=8",
      "--duration=30", "--warmup=2", "--ramp-up-ms=100", "--tail-ms=1500", "--loss=0",
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
  EXPECT_EQ(cfg->fanout_conns, 16u);
  EXPECT_EQ(cfg->conn_id_offset, 8u);
  EXPECT_EQ(cfg->duration_s, 30u);
  EXPECT_EQ(cfg->warmup_s, 2u);
  EXPECT_EQ(cfg->ramp_up_ms, 100u);
  EXPECT_EQ(cfg->tail_ms, 1500u);
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

TEST(Scenario, RejectsMalformedNumericFlags) {
  EXPECT_FALSE(Parses({"rudp-bench", "--library=raw_udp", "--rate-u=-1"}));
  EXPECT_FALSE(Parses({"rudp-bench", "--library=raw_udp", "--rate-u= 1"}));
  EXPECT_FALSE(Parses({"rudp-bench", "--library=raw_udp", "--rate-u=10x"}));
  EXPECT_FALSE(Parses({"rudp-bench", "--library=raw_udp", "--rate-u="}));
  EXPECT_FALSE(Parses({"rudp-bench", "--library=raw_udp", "--rate-u=1",
                       "--port=70000"}));
  EXPECT_FALSE(Parses({"rudp-bench", "--library=raw_udp", "--rate-u=1",
                       "--loss=nan"}));
  EXPECT_FALSE(Parses({"rudp-bench", "--library=raw_udp", "--rate-u=1",
                       "--loss=101"}));
  EXPECT_FALSE(Parses({"rudp-bench", "--library=raw_udp", "--rate-u=1",
                       "--conns=0"}));
}

TEST(Scenario, AcceptsNumericBoundaryValues) {
  EXPECT_TRUE(Parses({"rudp-bench", "--library=raw_udp", "--rate-u=1",
                      "--port=65535", "--loss=100", "--conns=1"}));
}

TEST(Scenario, DefaultsFanoutConnsToLocalConns) {
  const char* argv[] = {
      "rudp-bench", "--library=raw_udp", "--role=client",
      "--rate-u=1", "--conns=7",
  };
  int argc = sizeof(argv) / sizeof(argv[0]);
  auto cfg = rudp_bench::parse_scenario(argc, argv);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->fanout_conns, 7u);
}
