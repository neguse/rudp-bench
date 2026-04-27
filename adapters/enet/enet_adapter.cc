#include "harness/adapter.h"
#include "harness/adapter_registry.h"

#include <enet/enet.h>

#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

  void server_listen(uint16_t port) override {
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    // 最大ピア数 4096(Phase 1 では conns ≤ 1000、余裕)、2 channel、帯域無制限
    host_ = enet_host_create(&addr, 4096, 2, 0, 0);
    if (!host_) std::abort();
    is_server_ = true;
  }

  uint32_t client_connect(const char* /*host*/, uint16_t /*port*/) override {
    std::abort();
  }

  bool is_connected(uint32_t conn_id) override {
    return connected_ids_.count(conn_id) > 0;
  }

  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    auto it = peer_by_id_.find(conn_id);
    if (it == peer_by_id_.end()) return -1;
    ENetPeer* peer = it->second;
    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* pkt = enet_packet_create(data, len, flags);
    if (!pkt) return -1;
    if (enet_peer_send(peer, 0, pkt) != 0) {
      enet_packet_destroy(pkt);
      return -1;
    }
    // ENet は host_service / host_flush で実際の送信。即時 flush して loopback latency を最小化。
    enet_host_flush(host_);
    return 0;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    if (inbox_.empty()) return 0;
    auto& m = inbox_.front();
    if (m.data.size() > cap) {
      // 切り詰めず破棄して err を返す。Phase 1 では cap=2048〜65536 想定
      inbox_.pop_front();
      return -1;
    }
    std::memcpy(buf, m.data.data(), m.data.size());
    *out_len = m.data.size();
    *out_conn_id = m.conn_id;
    inbox_.pop_front();
    return 1;
  }

  void poll() override {
    if (!host_) return;
    ENetEvent ev;
    while (enet_host_service(host_, &ev, 0) > 0) {
      switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT: {
          uint32_t id;
          auto it = id_by_peer_.find(ev.peer);
          if (it == id_by_peer_.end()) {
            id = next_id_++;
            id_by_peer_[ev.peer] = id;
            peer_by_id_[id] = ev.peer;
          } else {
            id = it->second;
          }
          connected_ids_.insert(id);
          break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
          auto it = id_by_peer_.find(ev.peer);
          uint32_t id;
          if (it == id_by_peer_.end()) {
            id = next_id_++;
            id_by_peer_[ev.peer] = id;
            peer_by_id_[id] = ev.peer;
          } else {
            id = it->second;
          }
          InboundMsg m;
          m.conn_id = id;
          m.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
          inbox_.push_back(std::move(m));
          enet_packet_destroy(ev.packet);
          break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
          auto it = id_by_peer_.find(ev.peer);
          if (it != id_by_peer_.end()) {
            uint32_t id = it->second;
            connected_ids_.erase(id);
            peer_by_id_.erase(id);
            id_by_peer_.erase(it);
          }
          break;
        }
        default: break;
      }
    }
  }

  void close() override {
    if (host_) { enet_host_destroy(host_); host_ = nullptr; }
  }

  const char* name() const override { return "enet"; }
  bool supports(bool /*reliable*/) const override { return true; }
  bool encryption_on() const override { return false; }

 private:
  struct InboundMsg {
    uint32_t conn_id;
    std::vector<uint8_t> data;
  };

  ENetHost* host_ = nullptr;
  bool is_server_ = false;

  // peer ↔ conn_id マッピング(双方向)
  std::unordered_map<ENetPeer*, uint32_t> id_by_peer_;
  std::unordered_map<uint32_t, ENetPeer*> peer_by_id_;
  std::unordered_set<uint32_t> connected_ids_;
  uint32_t next_id_ = 1;

  // 受信メッセージのキュー
  std::deque<InboundMsg> inbox_;
};

}  // namespace

namespace rudp_bench {
void register_enet_adapter() {
  register_adapter("enet",
      []() { return std::make_unique<EnetAdapter>(); });
}
}  // namespace rudp_bench
