#include <gtest/gtest.h>

#include "harness/adapter_registry.h"
#include "harness/runner.h"

#include <thread>

namespace rudp_bench { void register_raw_udp_adapter(); }

class RawUdpRegistrar {
 public:
  RawUdpRegistrar() { rudp_bench::register_raw_udp_adapter(); }
};
static RawUdpRegistrar registrar;

using namespace rudp_bench;

TEST(RunnerLoopback, RawUdpShortSession) {
  ScenarioConfig sc;
  sc.library = "raw_udp";
  sc.role = Role::Server;
  sc.port = 0xC110;
  sc.reliable = Reliability::Unreliable;
  sc.duration_s = 2;
  sc.warmup_s = 0;

  ScenarioConfig cc = sc;
  cc.role = Role::Client;
  cc.host = "127.0.0.1";
  cc.size_bytes = 64;
  cc.conns = 1;
  cc.rate_per_conn = 100;

  auto server = create_adapter("raw_udp");
  auto client = create_adapter("raw_udp");

  std::thread st([&]() { run_server(*server, sc); });
  // give server a beat
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CsvRow row = run_client(*client, cc);
  st.join();

  EXPECT_GT(row.sent, 100u);
  EXPECT_GT(row.delivered, 50u);
  EXPECT_GT(row.delivery_ratio, 0.5);
  EXPECT_GT(row.client_offered, 100u);
  EXPECT_EQ(row.client_tick_ok, 1u);
}
