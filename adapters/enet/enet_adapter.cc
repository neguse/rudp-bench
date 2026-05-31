#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <enet/enet.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// L14: bound the inbound queue so a slow consumer cannot grow harness RSS
// without limit. 65536 messages is generous headroom over the harness's
// per-poll drain (kServerRecvDrainLimit=1024); overflow drops oldest + counts.
constexpr size_t kEnetInboxLimit = 1u << 16;

// --- Optional pooling allocator (ENET_POOL=1, default on) ----------------
// enet does ~3 small mallocs per message (ENetPacket + data buffer +
// ENetOutgoingCommand) plus per-ack ENetAcknowledgement; at high conns that
// malloc/free churn dominates the single-thread CPU that saturates at 1000conn.
// Replace glibc malloc with thread-local, size-segregated free-lists (O(1),
// lock-free in the single-threaded benchmark) via enet_initialize_with_callbacks
// — no submodule change. A size_t header before each block records the rounded
// size so free() can find the right bucket (0 = oversize raw malloc).
namespace enet_pool {
constexpr size_t kAlign = 16;        // header + user alignment
constexpr size_t kMaxPooled = 4096;  // larger -> raw malloc
constexpr size_t kNBuckets = kMaxPooled / kAlign + 1;
thread_local void* t_free[kNBuckets] = {nullptr};

inline size_t roundup(size_t s) { return (s + kAlign - 1) & ~(kAlign - 1); }

void* ealloc(size_t size) {
  if (size == 0) size = 1;
  if (size > kMaxPooled) {
    char* p = static_cast<char*>(std::malloc(kAlign + size));
    if (!p) return nullptr;
    *reinterpret_cast<size_t*>(p) = 0;  // 0 = raw
    return p + kAlign;
  }
  size_t r = roundup(size);
  size_t b = r / kAlign;
  if (void* h = t_free[b]) {
    t_free[b] = *reinterpret_cast<void**>(h);
    return h;
  }
  char* p = static_cast<char*>(std::malloc(kAlign + r));
  if (!p) return nullptr;
  *reinterpret_cast<size_t*>(p) = r;
  return p + kAlign;
}

void efree(void* mem) {
  if (!mem) return;
  char* p = static_cast<char*>(mem) - kAlign;
  size_t r = *reinterpret_cast<size_t*>(p);
  if (r == 0) { std::free(p); return; }
  size_t b = r / kAlign;
  *reinterpret_cast<void**>(mem) = t_free[b];
  t_free[b] = mem;
}

void enomem() { std::abort(); }
}  // namespace enet_pool

inline bool enet_pool_enabled() {
  static const bool on = []() {
    const char* v = std::getenv("ENET_POOL");
    return v ? std::atoi(v) != 0 : true;  // default ON
  }();
  return on;
}

// Socket SO_RCVBUF/SO_SNDBUF = 256KB, matching enet's own internal default and
// the other UDP adapters (L17). A 2026-05-31 clean A/B found a bigger buffer does
// NOT help enet at the 1000conn saturation knee (0.589 vs 0.588 at 1MB — the
// earlier "buffer helps" was run-to-run noise). ENET_RCVBUF_KB overrides for
// sweeps; default 256 keeps the field even and avoids the kcp-style bufferbloat.
inline void enet_apply_socket_buf(ENetHost* host) {
  int kb = 256;
  if (const char* v = std::getenv("ENET_RCVBUF_KB"); v && *v) {
    int e = std::atoi(v);
    if (e > 0) kb = e;
  }
  int bytes = kb * 1024;
  enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, bytes);
  enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF, bytes);
}

void ensure_enet_init() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    int rc;
    if (enet_pool_enabled()) {
      ENetCallbacks cb;
      std::memset(&cb, 0, sizeof(cb));
      cb.malloc = enet_pool::ealloc;
      cb.free = enet_pool::efree;
      cb.no_memory = enet_pool::enomem;
      rc = enet_initialize_with_callbacks(ENET_VERSION, &cb);
    } else {
      rc = enet_initialize();
    }
    if (rc != 0) std::abort();
    std::atexit([]() { enet_deinitialize(); });
  });
}

class EnetAdapter : public rudp_bench::Adapter {
 public:
  EnetAdapter() {
    ensure_enet_init();
    inbox_.set_limit(kEnetInboxLimit);  // L14
  }
  ~EnetAdapter() override {
    if (host_) enet_host_destroy(host_);
  }

  void hint_connections(uint32_t n) override { hint_conns_ = n; }

  void server_listen(uint16_t port) override {
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    // peerCount を実 conn 数に合わせる。enet_protocol_send_outgoing_commands /
    // bandwidth_throttle は毎 flush で host->peers の全スロットを線形走査するため、
    // 4095 固定だと実 conn 数に関係ない固定 CPU 税が乗る(高 conns の単コア飽和を
    // 早める)。上限は ENET_PROTOCOL_MAXIMUM_PEER_ID=0xFFF=4095。
    host_ = enet_host_create(&addr, peer_count(), 2, 0, 0);
    if (!host_) std::abort();
    enet_apply_socket_buf(host_);
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    if (!host_) {
      host_ = enet_host_create(nullptr, peer_count(), 2, 0, 0);
      if (!host_) std::abort();
      enet_apply_socket_buf(host_);
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
    // channel 0 = reliable ordered, channel 1 = unreliable unsequenced.
    // UNSEQUENCED is required so unreliable packets do not block behind
    // pending reliable retransmits on the channel-level sequence number.
    uint8_t channel;
    uint32_t flags;
    if (reliable) {
      channel = 0;
      flags = ENET_PACKET_FLAG_RELIABLE;
    } else {
      channel = 1;
      flags = ENET_PACKET_FLAG_UNSEQUENCED;
    }
    ENetPacket* pkt = enet_packet_create(data, len, flags);
    if (!pkt) return -1;
    if (enet_peer_send(peer, channel, pkt) != 0) {
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
    if (host_) {
      // L14: surface any bounded-inbox drops (captured into stderr_path).
      if (inbox_.dropped() > 0) {
        std::fprintf(stderr, "enet_inbox_dropped: %llu\n",
                     (unsigned long long)inbox_.dropped());
        std::fflush(stderr);
      }
      enet_host_destroy(host_);
      host_ = nullptr;
    }
  }

  // L15: ENet already sets SO_RCVBUF/SO_SNDBUF to 256KB on its internal socket
  // (third_party/enet/host.c:65-66, ENET_HOST_RECEIVE_BUFFER_SIZE = 256*1024),
  // so it is on the same socket-buffer footing as raw_udp — no extra tuning
  // needed here. mini_rudp/kcp are brought up to the same 256KB to even the
  // baseline (L17).

  const char* name() const override { return "enet"; }
  bool supports(bool /*reliable*/) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override { return 65536; }
  uint32_t max_connections() const override { return 4095; }
  const char* flush_policy(bool /*reliable*/) const override { return "poll_flush"; }
  bool encryption_on() const override { return false; }

 private:
  // 実 conn 数 + 余裕(12.5% + 8)を ENet 上限 4095 でクランプ。hint 無し(0)なら
  // 従来どおり 4095。client/server とも同じ計算。
  size_t peer_count() const {
    if (hint_conns_ == 0) return 4095;
    uint32_t want = hint_conns_ + hint_conns_ / 8 + 8;
    return want > 4095 ? 4095 : want;
  }

  uint32_t hint_conns_ = 0;
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
