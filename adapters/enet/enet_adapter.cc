#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

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
    // 最大ピア数 4095(ENet プロトコル上限 ENET_PROTOCOL_MAXIMUM_PEER_ID=0xFFF)、2 channel、帯域無制限
    host_ = enet_host_create(&addr, 4095, 2, 0, 0);
    if (!host_) std::abort();
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    if (!host_) {
      // 同一クライアントから複数 peer を張る用途で 4095 (ENet 上限) を確保。
      // 32 にすると 33 本目の enet_host_connect が NULL → abort する。
      host_ = enet_host_create(nullptr, 4095, 2, 0, 0);
      if (!host_) std::abort();
    }
    ENetAddress addr{};
    enet_address_set_host(&addr, host);
    addr.port = port;
    ENetPeer* peer = enet_host_connect(host_, &addr, 2, 0);
    if (!peer) std::abort();
    uint32_t id = next_id_++;
    id_by_peer_[peer] = id;
    peer_by_id_[id] = peer;
    // CONNECT イベントが来るまで connected_ids_ に入らない
    return id;
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
    // ENet は poll() 末尾で flush する(バッチング維持)
    return 0;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    return inbox_.recv(buf, cap, out_len, out_conn_id);
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
          inbox_.enqueue(id, ev.packet->data, ev.packet->dataLength);
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
    enet_host_flush(host_);
  }

  void close() override {
    if (host_) { enet_host_destroy(host_); host_ = nullptr; }
  }

  const char* name() const override { return "enet"; }
  bool supports(bool /*reliable*/) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override { return 65536; }
  uint32_t max_connections() const override { return 4095; }
  const char* flush_policy(bool /*reliable*/) const override { return "poll_flush"; }
  bool encryption_on() const override { return false; }

 private:
  ENetHost* host_ = nullptr;

  // peer ↔ conn_id マッピング(双方向)
  std::unordered_map<ENetPeer*, uint32_t> id_by_peer_;
  std::unordered_map<uint32_t, ENetPeer*> peer_by_id_;
  std::unordered_set<uint32_t> connected_ids_;
  uint32_t next_id_ = 1;

  // 受信メッセージのキュー
  rudp_bench::ReusableInboundQueue inbox_;
};

}  // namespace

namespace rudp_bench {
void register_enet_adapter() {
  register_adapter("enet",
      []() { return std::make_unique<EnetAdapter>(); });
}
}  // namespace rudp_bench
