#include <gtest/gtest.h>
#include "harness/adapter.h"
#include "harness/adapter_registry.h"

namespace {

class FakeAdapter : public rudp_bench::Adapter {
 public:
  void server_listen(uint16_t) override {}
  uint32_t client_connect(const char*, uint16_t) override { return 0; }
  bool is_connected(uint32_t) override { return true; }
  int send(uint32_t, const void*, size_t, bool) override { return 0; }
  int recv(void*, size_t, size_t*, uint32_t*) override { return 0; }
  void poll() override {}
  void close() override {}
  const char* name() const override { return "fake"; }
  bool supports(bool) const override { return true; }
  bool encryption_on() const override { return false; }
};

TEST(AdapterRegistry, RegistersAndCreates) {
  rudp_bench::register_adapter("fake", []() { return std::make_unique<FakeAdapter>(); });
  auto a = rudp_bench::create_adapter("fake");
  ASSERT_NE(a, nullptr);
  EXPECT_STREQ(a->name(), "fake");
}

TEST(AdapterRegistry, UnknownReturnsNull) {
  EXPECT_EQ(rudp_bench::create_adapter("does-not-exist"), nullptr);
}

}  // namespace
