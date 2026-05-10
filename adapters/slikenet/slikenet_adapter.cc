#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <slikenet/MessageIdentifiers.h>
#include <slikenet/PacketPriority.h>
#include <slikenet/peerinterface.h>
#include <slikenet/types.h>

#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

class SLikeNetAdapter : public rudp_bench::Adapter {
 public:
  SLikeNetAdapter() {
    peer_ = SLNet::RakPeerInterface::GetInstance();
  }

  ~SLikeNetAdapter() override {
    if (peer_) {
      peer_->Shutdown(100);
      SLNet::RakPeerInterface::DestroyInstance(peer_);
    }
  }

  void server_listen(uint16_t port) override {
    // サーバモード: 指定ポートで Startup し、着信コネクションを許可
    SLNet::SocketDescriptor sd(port, nullptr);
    peer_->Startup(4096, &sd, 1);
    peer_->SetMaximumIncomingConnections(4096);
    started_ = true;
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    if (!started_) {
      // クライアントモード: OS に割り当てさせたポートで Startup
      SLNet::SocketDescriptor sd(0, nullptr);
      peer_->Startup(4096, &sd, 1);
      started_ = true;
    }
    uint32_t id = next_id_++;
    auto res = peer_->Connect(host, port, nullptr, 0);
    if (res == SLNet::CONNECTION_ATTEMPT_STARTED ||
        res == SLNet::CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS) {
      // 非同期でコネクション確立待ち
      pending_ids_.push_back(id);
    } else if (res == SLNet::ALREADY_CONNECTED_TO_ENDPOINT) {
      // 同じアドレスへの重複接続: 既存コネクションの GUID が確定したら一括 resolve
      pending_ids_.push_back(id);
    } else {
      // 接続開始失敗: ID を返すが is_connected は false のまま
    }
    return id;
  }

  bool is_connected(uint32_t conn_id) override {
    return connected_ids_.count(conn_id) > 0;
  }

  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    auto it = guid_by_id_.find(conn_id);
    if (it == guid_by_id_.end()) return -1;

    // SLikeNet は data[0] をメッセージ識別子として使う。
    // ユーザデータの前に ID_USER_PACKET_ENUM を 1 byte 付加してシステムメッセージと区別する。
    std::vector<char> buf(len + 1);
    buf[0] = static_cast<char>(ID_USER_PACKET_ENUM);
    std::memcpy(buf.data() + 1, data, len);

    peer_->Send(buf.data(), static_cast<int>(buf.size()),
                HIGH_PRIORITY,
                reliable ? RELIABLE_ORDERED : UNRELIABLE,
                0,
                SLNet::AddressOrGUID(it->second),
                false);
    return 0;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
    if (!peer_) return;
    SLNet::Packet* p;
    while ((p = peer_->Receive()) != nullptr) {
      switch (p->data[0]) {
        case ID_NEW_INCOMING_CONNECTION: {
          // サーバ側: クライアントからの新規接続
          uint32_t id = register_guid(p->guid);
          connected_ids_.insert(id);
          break;
        }
        case ID_CONNECTION_REQUEST_ACCEPTED: {
          // クライアント側: Connect() が受理された
          // pending_ids_ の先頭からすべて同一 GUID にマップして connected にする
          // (同一サーバへの複数 client_connect() は同一物理コネクションを共有)
          for (uint32_t pid : pending_ids_) {
            guid_by_id_[pid] = p->guid;
            id_by_guid_[p->guid.g] = pid;
            connected_ids_.insert(pid);
          }
          pending_ids_.clear();
          break;
        }
        case ID_DISCONNECTION_NOTIFICATION:
        case ID_CONNECTION_LOST: {
          auto it = id_by_guid_.find(p->guid.g);
          if (it != id_by_guid_.end()) {
            uint32_t id = it->second;
            connected_ids_.erase(id);
            guid_by_id_.erase(id);
            id_by_guid_.erase(it);
          }
          break;
        }
        case ID_USER_PACKET_ENUM: {
          // ユーザデータ: data[0] はヘッダ識別子なので data[1..] が実ペイロード
          if (p->length > 1) {
            uint32_t id = get_or_register_guid(p->guid);
            inbox_.enqueue(id, p->data + 1, p->length - 1);
          }
          break;
        }
        default: break;
      }
      peer_->DeallocatePacket(p);
    }
  }

  void close() override {
    if (peer_) {
      peer_->Shutdown(300);
      SLNet::RakPeerInterface::DestroyInstance(peer_);
      peer_ = nullptr;
    }
  }

  const char* name() const override { return "slikenet"; }
  bool supports(bool /*reliable*/) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override { return 65536; }
  uint32_t max_connections() const override { return 1; }
  const char* flush_policy(bool /*reliable*/) const override { return "library_internal"; }
  bool encryption_on() const override { return false; }

 private:
  // GUID を新規登録して conn_id を返す
  uint32_t register_guid(const SLNet::RakNetGUID& guid) {
    uint32_t id = next_id_++;
    guid_by_id_[id] = guid;
    id_by_guid_[guid.g] = id;
    return id;
  }

  // すでに登録済みなら既存 ID、なければ新規登録して返す
  uint32_t get_or_register_guid(const SLNet::RakNetGUID& guid) {
    auto it = id_by_guid_.find(guid.g);
    if (it != id_by_guid_.end()) return it->second;
    return register_guid(guid);
  }

  SLNet::RakPeerInterface* peer_ = nullptr;
  bool started_ = false;

  // conn_id ↔ GUID 双方向マッピング
  std::unordered_map<uint32_t, SLNet::RakNetGUID> guid_by_id_;
  std::unordered_map<uint64_t, uint32_t> id_by_guid_;
  std::unordered_set<uint32_t> connected_ids_;
  uint32_t next_id_ = 1;

  // client_connect() 発行済みで GUID 未確定の conn_id キュー
  std::deque<uint32_t> pending_ids_;

  // 受信メッセージのキュー
  rudp_bench::ReusableInboundQueue inbox_;
};

}  // namespace

namespace rudp_bench {
void register_slikenet_adapter() {
  register_adapter("slikenet",
      []() { return std::make_unique<SLikeNetAdapter>(); });
}
}  // namespace rudp_bench
