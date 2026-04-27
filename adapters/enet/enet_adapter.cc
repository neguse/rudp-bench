#include "harness/adapter.h"
#include "harness/adapter_registry.h"

#include <enet/enet.h>

#include <cstring>
#include <mutex>

namespace {

void ensure_enet_init() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    if (enet_initialize() != 0) {
      std::abort();
    }
    std::atexit([]() { enet_deinitialize(); });
  });
}

class EnetAdapter : public rudp_bench::Adapter {
 public:
  EnetAdapter() { ensure_enet_init(); }
  ~EnetAdapter() override {
    if (host_) enet_host_destroy(host_);
  }

  void server_listen(uint16_t /*port*/) override {
    // Task 5 で実装
    std::abort();
  }
  uint32_t client_connect(const char* /*host*/, uint16_t /*port*/) override {
    std::abort();
  }
  bool is_connected(uint32_t /*conn_id*/) override { return false; }
  int send(uint32_t /*conn_id*/, const void* /*data*/, size_t /*len*/, bool /*reliable*/) override {
    return -1;
  }
  int recv(void* /*buf*/, size_t /*cap*/, size_t* /*out_len*/, uint32_t* /*out_conn_id*/) override {
    return 0;
  }
  void poll() override {}
  void close() override {
    if (host_) { enet_host_destroy(host_); host_ = nullptr; }
  }

  const char* name() const override { return "enet"; }
  bool supports(bool /*reliable*/) const override { return true; }
  bool encryption_on() const override { return false; }

 private:
  ENetHost* host_ = nullptr;
};

}  // namespace

namespace rudp_bench {
void register_enet_adapter() {
  register_adapter("enet",
      []() { return std::make_unique<EnetAdapter>(); });
}
}  // namespace rudp_bench
