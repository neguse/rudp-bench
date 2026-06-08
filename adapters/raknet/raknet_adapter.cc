#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <slikenet/MessageIdentifiers.h>
#include <slikenet/PacketPriority.h>
#include <slikenet/peerinterface.h>
#include <slikenet/types.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kMaxRakNetConnections = 4096;
constexpr size_t kMaxRakNetPayloadBytes = 65536;

struct ClientPeer {
  uint32_t id = 0;
  SLNet::RakPeerInterface* peer = nullptr;
  SLNet::RakNetGUID guid{};
  bool connected = false;
  bool failed = false;
};

class RakNetAdapter : public rudp_bench::Adapter {
 public:
  ~RakNetAdapter() override { close(); }

  void hint_connections(uint32_t n) override {
    hinted_connections_ = std::min<uint32_t>(std::max<uint32_t>(n, 1),
                                             kMaxRakNetConnections);
  }

  void server_listen(uint16_t port) override {
    close();
    is_server_ = true;
    peer_ = SLNet::RakPeerInterface::GetInstance();
    SLNet::SocketDescriptor sd(port, nullptr);
    unsigned int max_peers = hinted_connections_;
    SLNet::StartupResult sr = peer_->Startup(max_peers, &sd, 1);
    if (sr != SLNet::RAKNET_STARTED && sr != SLNet::RAKNET_ALREADY_STARTED) {
      std::abort();
    }
    peer_->SetMaximumIncomingConnections(
        static_cast<unsigned short>(max_peers));
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    is_server_ = false;
    uint32_t id = next_id_++;
    ClientPeer slot;
    slot.id = id;

    if (host == nullptr || client_peers_.size() >= kMaxRakNetConnections) {
      slot.failed = true;
      client_peers_.push_back(slot);
      peer_index_by_id_[id] = client_peers_.size() - 1;
      return id;
    }

    slot.peer = SLNet::RakPeerInterface::GetInstance();
    SLNet::SocketDescriptor sd(0, nullptr);
    SLNet::StartupResult sr = slot.peer->Startup(1, &sd, 1);
    if (sr != SLNet::RAKNET_STARTED && sr != SLNet::RAKNET_ALREADY_STARTED) {
      SLNet::RakPeerInterface::DestroyInstance(slot.peer);
      slot.peer = nullptr;
      slot.failed = true;
      client_peers_.push_back(slot);
      peer_index_by_id_[id] = client_peers_.size() - 1;
      return id;
    }

    auto cr = slot.peer->Connect(host, port, nullptr, 0);
    if (cr != SLNet::CONNECTION_ATTEMPT_STARTED &&
        cr != SLNet::CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS &&
        cr != SLNet::ALREADY_CONNECTED_TO_ENDPOINT) {
      slot.failed = true;
    }

    client_peers_.push_back(slot);
    peer_index_by_id_[id] = client_peers_.size() - 1;
    return id;
  }

  bool is_connected(uint32_t conn_id) override {
    if (is_server_) return connected_ids_.count(conn_id) > 0;
    ClientPeer* slot = find_client_peer(conn_id);
    if (!slot) return false;
    return slot->connected || slot->failed;
  }

  int send(uint32_t conn_id, const void* data, size_t len,
           bool reliable) override {
    if ((!data && len != 0) || len > kMaxRakNetPayloadBytes) return -1;
    build_packet(data, len);
    if (is_server_) {
      auto it = guid_by_id_.find(conn_id);
      if (it == guid_by_id_.end()) return -1;
      return send_packet(peer_, it->second, reliable);
    }
    ClientPeer* slot = find_client_peer(conn_id);
    if (!slot || !slot->peer || !slot->connected || slot->failed) return -1;
    return send_packet(slot->peer, slot->guid, reliable);
  }

  size_t send_many(const uint32_t* conn_ids, size_t count, const void* data,
                   size_t len, bool reliable) override {
    if (!is_server_) {
      return rudp_bench::Adapter::send_many(conn_ids, count, data, len,
                                            reliable);
    }
    if (!peer_ || (!conn_ids && count != 0) || (!data && len != 0) ||
        len > kMaxRakNetPayloadBytes) {
      return 0;
    }
    build_packet(data, len);
    size_t accepted = 0;
    for (size_t i = 0; i < count; ++i) {
      auto it = guid_by_id_.find(conn_ids[i]);
      if (it == guid_by_id_.end()) continue;
      if (send_packet(peer_, it->second, reliable) == 0) ++accepted;
    }
    return accepted;
  }

  int recv(void* buf, size_t cap, size_t* out_len,
           uint32_t* out_conn_id) override {
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
    if (is_server_) {
      poll_server();
      return;
    }
    for (auto& slot : client_peers_) {
      poll_client(slot);
    }
  }

  void close() override {
    if (peer_) {
      peer_->Shutdown(100);
      SLNet::RakPeerInterface::DestroyInstance(peer_);
      peer_ = nullptr;
    }
    for (auto& slot : client_peers_) {
      if (!slot.peer) continue;
      slot.peer->Shutdown(100);
      SLNet::RakPeerInterface::DestroyInstance(slot.peer);
      slot.peer = nullptr;
    }
    client_peers_.clear();
    peer_index_by_id_.clear();
    guid_by_id_.clear();
    id_by_guid_.clear();
    connected_ids_.clear();
    connected_peak_ = 0;
    shutdown_by_transport_ = 0;
    shutdown_by_peer_ = 0;
    is_server_ = false;
  }

  const char* name() const override { return "raknet"; }
  bool supports(bool /*reliable*/) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override {
    return kMaxRakNetPayloadBytes;
  }
  uint32_t max_connections() const override { return kMaxRakNetConnections; }
  const char* flush_policy(bool /*reliable*/) const override {
    return "library_internal";
  }
  bool encryption_on() const override { return false; }

  rudp_bench::ConnectionStats connection_stats() const override {
    rudp_bench::ConnectionStats stats;
    stats.connected_peak = connected_peak_;
    stats.shutdown_by_transport = shutdown_by_transport_;
    stats.shutdown_by_peer = shutdown_by_peer_;
    return stats;
  }

 private:
  ClientPeer* find_client_peer(uint32_t id) {
    auto it = peer_index_by_id_.find(id);
    if (it == peer_index_by_id_.end()) return nullptr;
    if (it->second >= client_peers_.size()) return nullptr;
    return &client_peers_[it->second];
  }

  void build_packet(const void* data, size_t len) {
    send_scratch_.resize(len + 1);
    send_scratch_[0] = static_cast<char>(ID_USER_PACKET_ENUM);
    if (len != 0) {
      std::memcpy(send_scratch_.data() + 1, data, len);
    }
  }

  int send_packet(SLNet::RakPeerInterface* peer,
                  const SLNet::RakNetGUID& guid, bool reliable) {
    if (!peer) return -1;
    uint32_t receipt = peer->Send(send_scratch_.data(),
                                  static_cast<int>(send_scratch_.size()),
                                  HIGH_PRIORITY,
                                  reliable ? RELIABLE_ORDERED : UNRELIABLE,
                                  0,
                                  SLNet::AddressOrGUID(guid),
                                  false);
    return receipt == 0 ? -1 : 0;
  }

  uint32_t register_guid(const SLNet::RakNetGUID& guid) {
    auto existing = id_by_guid_.find(guid.g);
    if (existing != id_by_guid_.end()) return existing->second;
    uint32_t id = next_id_++;
    guid_by_id_[id] = guid;
    id_by_guid_[guid.g] = id;
    connected_ids_.insert(id);
    connected_peak_ = std::max<uint32_t>(
        connected_peak_, static_cast<uint32_t>(connected_ids_.size()));
    return id;
  }

  void remove_guid(const SLNet::RakNetGUID& guid, bool by_transport) {
    auto it = id_by_guid_.find(guid.g);
    if (it == id_by_guid_.end()) return;
    uint32_t id = it->second;
    id_by_guid_.erase(it);
    guid_by_id_.erase(id);
    connected_ids_.erase(id);
    if (by_transport) {
      ++shutdown_by_transport_;
    } else {
      ++shutdown_by_peer_;
    }
  }

  void handle_user_packet(SLNet::Packet* p, uint32_t id) {
    if (!p || p->length <= 1) return;
    inbox_.enqueue(id, p->data + 1, p->length - 1);
  }

  void poll_server() {
    if (!peer_) return;
    SLNet::Packet* p = nullptr;
    while ((p = peer_->Receive()) != nullptr) {
      if (p->length == 0) {
        peer_->DeallocatePacket(p);
        continue;
      }
      switch (p->data[0]) {
        case ID_NEW_INCOMING_CONNECTION:
          register_guid(p->guid);
          break;
        case ID_DISCONNECTION_NOTIFICATION:
          remove_guid(p->guid, false);
          break;
        case ID_CONNECTION_LOST:
          remove_guid(p->guid, true);
          break;
        case ID_USER_PACKET_ENUM:
          handle_user_packet(p, register_guid(p->guid));
          break;
        default:
          break;
      }
      peer_->DeallocatePacket(p);
    }
  }

  void poll_client(ClientPeer& slot) {
    if (!slot.peer) return;
    SLNet::Packet* p = nullptr;
    while ((p = slot.peer->Receive()) != nullptr) {
      if (p->length == 0) {
        slot.peer->DeallocatePacket(p);
        continue;
      }
      switch (p->data[0]) {
        case ID_CONNECTION_REQUEST_ACCEPTED:
        case ID_ALREADY_CONNECTED:
          slot.guid = p->guid;
          slot.connected = true;
          slot.failed = false;
          connected_ids_.insert(slot.id);
          connected_peak_ = std::max<uint32_t>(
              connected_peak_, static_cast<uint32_t>(connected_ids_.size()));
          break;
        case ID_CONNECTION_ATTEMPT_FAILED:
        case ID_NO_FREE_INCOMING_CONNECTIONS:
        case ID_CONNECTION_BANNED:
        case ID_INVALID_PASSWORD:
          slot.failed = true;
          slot.connected = false;
          connected_ids_.erase(slot.id);
          break;
        case ID_DISCONNECTION_NOTIFICATION:
          slot.connected = false;
          connected_ids_.erase(slot.id);
          ++shutdown_by_peer_;
          break;
        case ID_CONNECTION_LOST:
          slot.connected = false;
          connected_ids_.erase(slot.id);
          ++shutdown_by_transport_;
          break;
        case ID_USER_PACKET_ENUM:
          if (slot.connected) handle_user_packet(p, slot.id);
          break;
        default:
          break;
      }
      slot.peer->DeallocatePacket(p);
    }
  }

  SLNet::RakPeerInterface* peer_ = nullptr;
  bool is_server_ = false;
  uint32_t hinted_connections_ = kMaxRakNetConnections;
  uint32_t next_id_ = 1;

  std::vector<ClientPeer> client_peers_;
  std::unordered_map<uint32_t, size_t> peer_index_by_id_;
  std::unordered_map<uint32_t, SLNet::RakNetGUID> guid_by_id_;
  std::unordered_map<uint64_t, uint32_t> id_by_guid_;
  std::unordered_set<uint32_t> connected_ids_;
  std::vector<char> send_scratch_;
  rudp_bench::ReusableInboundQueue inbox_;
  uint32_t connected_peak_ = 0;
  uint32_t shutdown_by_transport_ = 0;
  uint32_t shutdown_by_peer_ = 0;
};

}  // namespace

namespace rudp_bench {
void register_raknet_adapter() {
  register_adapter("raknet", []() { return std::make_unique<RakNetAdapter>(); });
}
}  // namespace rudp_bench
