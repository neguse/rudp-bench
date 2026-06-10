// RakNet 系 (bundled SLikeNet / RakNetLibStatic, RakNet 4.082 derived) の共通
// adapter 実装。"raknet" と "slikenet" の両 target がこのクラスを使う。
//
// RakNet の接続モデルは remote SystemAddress 単位なので、1 つの RakPeer から
// 同一 server への多重接続は張れない。クライアント側は論理 connection ごとに
// RakPeerInterface を 1 つ持つ(= conn ごとに socket + update thread)。
//
// 旧 raknet adapter からの修正点:
// - GUID 衝突由来のゾンビ接続を排除する。SLikeNet の POSIX 実装は
//   RakNetGUID = gettimeofday() のマイクロ秒値で、同時起動した client process
//   群が同一 GUID の RakPeer を高頻度で生成する。server は OCR2 で GUID 重複を
//   検出すると ID_ALREADY_CONNECTED で接続を拒否する ("someone else took this
//   guid") が、旧 adapter はこれを「接続成功」として扱っていたため、client は
//   connected・server 側には存在しない接続が測定に混入していた (canonical の
//   game_server c5 で 5 本中 1 本死亡 → delivery 0.63 の run、valid_runs=1/3
//   での aggregate invalid の正体)。GUID は Startup 時に 1 回だけ生成されるの
//   で、同じ peer での Connect リトライは永遠に拒否される。よって接続失敗時は
//   RakPeer を破棄して作り直す (新 Startup → 新タイムスタンプ GUID)。
// - 接続失敗 slot を is_connected()=true で偽装しない。回復は上記の再生成で行う。
// - server の Startup/SetMaximumIncomingConnections にヘッドルームを持たせる。
//   ハンドシェイク中の半開き slot が定員を食い潰すと、同時 connect 嵐 + netem
//   loss で ID_NO_FREE_INCOMING_CONNECTIONS が出る。
// - Connect の再送パラメータを明示 (12 attempts x 250ms)。既定 (12 x 500ms) は
//   1 サイクルの失敗確定までが長く、リトライ回復が遅い。
#pragma once

#include "harness/adapter.h"
#include "harness/inbound_queue.h"

#include <slikenet/BitStream.h>
#include <slikenet/MessageIdentifiers.h>
#include <slikenet/PacketPriority.h>
#include <slikenet/peerinterface.h>
#include <slikenet/types.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rak_family {

constexpr uint32_t kMaxConnections = 4096;
constexpr size_t kMaxPayloadBytes = 65536;
constexpr unsigned kConnectAttempts = 12;
constexpr unsigned kConnectAttemptIntervalMs = 250;

struct ClientPeer {
  uint32_t id = 0;
  SLNet::RakPeerInterface* peer = nullptr;
  SLNet::RakNetGUID guid{};
  bool connected = false;
  // Connect() サイクルが進行中 (結果待ち)。false かつ未接続なら poll() が再発行する。
  bool connect_in_flight = false;
};

class RakFamilyAdapter : public rudp_bench::Adapter {
 public:
  explicit RakFamilyAdapter(const char* name) : name_(name) {}
  ~RakFamilyAdapter() override { close(); }

  void hint_connections(uint32_t n) override {
    hinted_connections_ =
        std::min<uint32_t>(std::max<uint32_t>(n, 1), kMaxConnections);
  }

  void server_listen(uint16_t port) override {
    close();
    is_server_ = true;
    peer_ = SLNet::RakPeerInterface::GetInstance();
    SLNet::SocketDescriptor sd(port, nullptr);
    // 接続済み定員 + ハンドシェイク用ヘッドルーム。
    uint32_t max_peers = std::min<uint32_t>(
        kMaxConnections,
        hinted_connections_ + std::max<uint32_t>(8, hinted_connections_ / 4));
    SLNet::StartupResult sr =
        peer_->Startup(max_peers, &sd, 1);
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

    if (host == nullptr || client_peers_.size() >= kMaxConnections) {
      client_peers_.push_back(slot);
      peer_index_by_id_[id] = client_peers_.size() - 1;
      return id;
    }
    host_ = host;
    port_ = port;

    slot.peer = SLNet::RakPeerInterface::GetInstance();
    SLNet::SocketDescriptor sd(0, nullptr);
    SLNet::StartupResult sr = slot.peer->Startup(1, &sd, 1);
    if (sr != SLNet::RAKNET_STARTED && sr != SLNet::RAKNET_ALREADY_STARTED) {
      SLNet::RakPeerInterface::DestroyInstance(slot.peer);
      slot.peer = nullptr;
      client_peers_.push_back(slot);
      peer_index_by_id_[id] = client_peers_.size() - 1;
      return id;
    }

    slot.connect_in_flight = try_connect(slot.peer);
    client_peers_.push_back(slot);
    peer_index_by_id_[id] = client_peers_.size() - 1;
    return id;
  }

  bool is_connected(uint32_t conn_id) override {
    if (is_server_) return connected_ids_.count(conn_id) > 0;
    ClientPeer* slot = find_client_peer(conn_id);
    return slot != nullptr && slot->connected;
  }

  int send(uint32_t conn_id, const void* data, size_t len,
           bool reliable) override {
    if ((!data && len != 0) || len > kMaxPayloadBytes) return -1;
    build_packet(data, len);
    if (is_server_) {
      auto it = guid_by_id_.find(conn_id);
      if (it == guid_by_id_.end()) return -1;
      return send_packet(peer_, it->second, reliable);
    }
    ClientPeer* slot = find_client_peer(conn_id);
    if (!slot || !slot->peer || !slot->connected) return -1;
    return send_packet(slot->peer, slot->guid, reliable);
  }

  size_t send_many(const uint32_t* conn_ids, size_t count, const void* data,
                   size_t len, bool reliable) override {
    if (!is_server_) {
      return rudp_bench::Adapter::send_many(conn_ids, count, data, len,
                                            reliable);
    }
    if (!peer_ || (!conn_ids && count != 0) || (!data && len != 0) ||
        len > kMaxPayloadBytes) {
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
    // RakNetLibStatic は RAKPEER_USER_THREADED=1 でビルドしている。update
    // thread は存在せず、ここで RunUpdateCycle を回す (送信 flush・ACK・再送・
    // 接続タイマーすべてこの中)。socket の recv thread が積んだ受信バッファも
    // ここで処理され Receive() に届く。
    if (is_server_) {
      if (peer_) peer_->RunUpdateCycle(update_bs_);
      poll_server();
      return;
    }
    for (auto& slot : client_peers_) {
      if (!slot.peer) {
        // 前回の recreate で Startup に失敗した slot を再試行する
        if (!host_.empty()) recreate_peer(slot);
        continue;
      }
      slot.peer->RunUpdateCycle(update_bs_);
      bool needs_recreate = poll_client(slot);
      if (needs_recreate) {
        recreate_peer(slot);
      } else if (!slot.connected && !slot.connect_in_flight) {
        slot.connect_in_flight = try_connect(slot.peer);
      }
    }
  }

  void close() override {
    // 切断通知は送る (相手側の conn_disc カウンタと port 解放のため): 接続を
    // 閉じて少しだけ update cycle を回して flush し、peer 自体は破棄せず放棄
    // する (abandon_peer のコメント参照)。
    if (peer_) {
      flush_disconnects(peer_);
      abandon_peer(peer_);
      peer_ = nullptr;
    }
    for (auto& slot : client_peers_) {
      if (!slot.peer) continue;
      if (slot.connected) {
        slot.peer->CloseConnection(SLNet::AddressOrGUID(slot.guid), true);
        flush_disconnects(slot.peer);
      }
      abandon_peer(slot.peer);
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

  const char* name() const override { return name_.c_str(); }
  bool supports(bool /*reliable*/) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override {
    return kMaxPayloadBytes;
  }
  uint32_t max_connections() const override { return kMaxConnections; }
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
  bool try_connect(SLNet::RakPeerInterface* peer) {
    auto cr = peer->Connect(host_.c_str(), port_, nullptr, 0,
                            /*publicKey=*/nullptr, /*socketIndex=*/0,
                            kConnectAttempts, kConnectAttemptIntervalMs,
                            /*timeoutTime=*/0);
    return cr == SLNet::CONNECTION_ATTEMPT_STARTED ||
           cr == SLNet::CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS ||
           cr == SLNet::ALREADY_CONNECTED_TO_ENDPOINT;
  }

  // RAKPEER_USER_THREADED=1 の upstream バグ対策: RakPeer を「破棄せず放棄」する。
  //
  // - Shutdown() の recv thread 停止処理 (SignalStop/BlockOnStopRecvPollingThread)
  //   は `#if RAKPEER_USER_THREADED!=1` ガード内にあるが、recv thread の生成は
  //   無条件。つまり素の Shutdown は recv thread を止めないまま socket と受信
  //   バッファを解放し、トラフィック下で use-after-free (SIGSEGV) する。
  // - 自前で止めようにも GetSockets()/GetSocket() は update thread へのクエリ
  //   として実装されており、update thread が居ない (isMainLoopThreadActive=false)
  //   と空リストで即 return するため、RNS2_Berkley に到達する公開経路が無い。
  //
  // よって Shutdown/DestroyInstance は呼ばず、ポインタを保持したまま放置する。
  // 放棄 peer は接続拒否済み (= server からのトラフィック無し) か process 終了
  // 直前のものだけなので、リークは有界で OS が回収する。
  void abandon_peer(SLNet::RakPeerInterface* peer) {
    abandoned_peers_.push_back(peer);
  }

  // CloseConnection の切断通知を wire に出すために少しだけ update cycle を回す
  void flush_disconnects(SLNet::RakPeerInterface* peer) {
    for (int i = 0; i < 5; ++i) {
      peer->RunUpdateCycle(update_bs_);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  // 接続失敗した client peer を放棄して作り直す。Startup ごとに GUID
  // (POSIX ではマイクロ秒タイムスタンプ) と ephemeral port が新しくなるので、
  // GUID 衝突・server 側の半開き残骸の両方から脱出できる。
  void recreate_peer(ClientPeer& slot) {
    if (slot.peer) {
      abandon_peer(slot.peer);
      slot.peer = nullptr;
    }
    slot.connected = false;
    slot.connect_in_flight = false;
    SLNet::RakPeerInterface* peer = SLNet::RakPeerInterface::GetInstance();
    SLNet::SocketDescriptor sd(0, nullptr);
    SLNet::StartupResult sr = peer->Startup(1, &sd, 1);
    if (sr != SLNet::RAKNET_STARTED && sr != SLNet::RAKNET_ALREADY_STARTED) {
      abandon_peer(peer);
      return;  // 次の poll() で再試行される (slot.peer == nullptr のまま)
    }
    slot.peer = peer;
    slot.connect_in_flight = try_connect(peer);
  }

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

  // 戻り値 true = この slot の RakPeer を作り直す必要がある (GUID 衝突拒否や
  // 確立失敗)。peer 破棄が必要なので packet ループは抜けてから呼び元で行う。
  bool poll_client(ClientPeer& slot) {
    if (!slot.peer) return false;
    SLNet::Packet* p = nullptr;
    while ((p = slot.peer->Receive()) != nullptr) {
      if (p->length == 0) {
        slot.peer->DeallocatePacket(p);
        continue;
      }
      switch (p->data[0]) {
        case ID_CONNECTION_REQUEST_ACCEPTED:
          slot.guid = p->guid;
          slot.connected = true;
          slot.connect_in_flight = false;
          connected_ids_.insert(slot.id);
          connected_peak_ = std::max<uint32_t>(
              connected_peak_, static_cast<uint32_t>(connected_ids_.size()));
          break;
        case ID_ALREADY_CONNECTED:
          // server の GUID 重複拒否 (or 半開き残骸への再接続)。この peer の
          // GUID では何度 Connect しても拒否されるので作り直す。
        case ID_CONNECTION_ATTEMPT_FAILED:
        case ID_NO_FREE_INCOMING_CONNECTIONS:
        case ID_CONNECTION_BANNED:
        case ID_INVALID_PASSWORD:
          slot.connected = false;
          slot.connect_in_flight = false;
          connected_ids_.erase(slot.id);
          slot.peer->DeallocatePacket(p);
          return true;
        case ID_DISCONNECTION_NOTIFICATION:
          slot.connected = false;
          slot.connect_in_flight = false;
          connected_ids_.erase(slot.id);
          ++shutdown_by_peer_;
          break;
        case ID_CONNECTION_LOST:
          slot.connected = false;
          slot.connect_in_flight = false;
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
    return false;
  }

  std::string name_;
  SLNet::RakPeerInterface* peer_ = nullptr;
  bool is_server_ = false;
  uint32_t hinted_connections_ = kMaxConnections;
  uint32_t next_id_ = 1;
  std::string host_;
  uint16_t port_ = 0;

  std::vector<ClientPeer> client_peers_;
  // 破棄すると upstream の USER_THREADED teardown バグで UAF するため、放棄した
  // RakPeer を保持し続ける (process 終了時に OS が回収)。abandon_peer 参照。
  std::vector<SLNet::RakPeerInterface*> abandoned_peers_;
  std::unordered_map<uint32_t, size_t> peer_index_by_id_;
  std::unordered_map<uint32_t, SLNet::RakNetGUID> guid_by_id_;
  std::unordered_map<uint64_t, uint32_t> id_by_guid_;
  std::unordered_set<uint32_t> connected_ids_;
  std::vector<char> send_scratch_;
  SLNet::BitStream update_bs_;  // RunUpdateCycle 用スクラッチ
  rudp_bench::ReusableInboundQueue inbox_;
  uint32_t connected_peak_ = 0;
  uint32_t shutdown_by_transport_ = 0;
  uint32_t shutdown_by_peer_ = 0;
};

}  // namespace rak_family
