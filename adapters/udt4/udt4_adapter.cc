#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

// UDT4 SDK 4.11 — vendored via FetchContent in adapters/udt4/CMakeLists.txt
#include <udt.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================
// UDT4 adapter design notes
// ============================================================
// UDT4 is stream-based (SOCK_STREAM over UDP).  To recover application
// message boundaries we prepend a 4-byte little-endian length field to
// every message.  recv() loops until the full frame is available.
//
// Unreliable mode is NOT supported by UDT4.  supports(false) returns false,
// and the harness emits a skipped row for scenarios with rate_u > 0.
//
// One UDTSOCKET per connection.  Server accept() loop runs inside poll().
// Recv is made non-blocking via UDT_RCVSYN=false; send also uses
// UDT_SNDSYN=false and keeps backpressured bytes in the adapter queue.
//
// Fork selection: loonycyborg/UDT-fixed → HTTP 404; eric-yhc/udt4 → HTTP 404;
// git HTTPS clone also unavailable in this environment.  System apt package
// libudt-dev 4.11+dfsg1 chosen (vendor approach option from the spec).
// ============================================================

#include <ccc.h>

namespace {

static constexpr UDTSOCKET kInvalidSock = -1;
static constexpr size_t kDefaultOutPendingByteLimit = 32u * 1024u * 1024u;

// UDT のデフォルト CC(CUDTCC)はギガビット級バルク転送向けのレート制御で、
// loss を見るたび送信周期を引き伸ばす。64B@50Hz のような小メッセージ流だと
// netem 1% loss 下で送信レートが offered rate を割り込み、送信バッファに
// 秒単位で滞留する(canonical で RTT p50 2.5s / delivery 0.31 の正体)。
// ベース CCC は「period 1us・固定 window 16・loss 無反応」の素朴な
// 窓制御で、この benchmark の message workload には十分かつ安定。
// window 16 / RTT 50ms ≈ 320 pkt/s/conn >> offered 50 pkt/s。
class BenchCCC : public CCC {
 public:
    BenchCCC() {
        m_dPktSndPeriod = 1.0;
        m_dCWndSize = 64.0;
    }
};

void ensure_udt_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        UDT::startup();
        std::atexit([]() { UDT::cleanup(); });
    });
}

size_t out_pending_byte_limit() {
    const char* v = std::getenv("UDT4_OUT_PENDING_BYTES");
    if (!v || !*v) return kDefaultOutPendingByteLimit;
    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v || *end != '\0' || errno == ERANGE || parsed == 0) {
        return kDefaultOutPendingByteLimit;
    }
    if (parsed > std::numeric_limits<size_t>::max()) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(parsed);
}

struct ConnState {
    UDTSOCKET sock = kInvalidSock;
    uint32_t id = 0;
    std::vector<uint8_t> partial;
    size_t partial_offset = 0;
    // 非ブロッキング送信で書き切れなかったバイト列(フレーム順保存)。
    // 消費は out_pending_offset を進めるだけ(front からの copy+erase による
    // O(n²) を回避)。offset が肥大したら amortized O(1) で前方を圧縮する。
    std::vector<uint8_t> out_pending;
    size_t out_pending_offset = 0;

    size_t out_pending_size() const {
        return out_pending.size() - out_pending_offset;
    }
    void compact_out_pending() {
        if (out_pending_offset == 0) return;
        if (out_pending_offset == out_pending.size()) {
            out_pending.clear();
            out_pending_offset = 0;
            return;
        }
        if (out_pending_offset > 4096 &&
            out_pending_offset * 2 >= out_pending.size()) {
            out_pending.erase(
                out_pending.begin(),
                out_pending.begin() +
                    static_cast<std::ptrdiff_t>(out_pending_offset));
            out_pending_offset = 0;
        }
    }
};

class Udt4Adapter : public rudp_bench::Adapter {
 public:
    Udt4Adapter() { ensure_udt_init(); }
    ~Udt4Adapter() override { close(); }

    // ---- server side -------------------------------------------------------

    void server_listen(uint16_t port) override {
        close();
        shutdown_by_transport_ = 0;
        is_server_ = true;
        listen_sock_ = make_socket();

        // Non-blocking accept so poll() can drain quickly.
        bool rcv_syn = false;
        UDT::setsockopt(listen_sock_, 0, UDT_RCVSYN, &rcv_syn, sizeof(rcv_syn));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (UDT::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) == UDT::ERROR)
            throw std::runtime_error("UDT::bind failed");
        if (UDT::listen(listen_sock_, 256) == UDT::ERROR)
            throw std::runtime_error("UDT::listen failed");

        ensure_epoll();
        int ev = UDT_EPOLL_IN;
        UDT::epoll_add_usock(eid_, listen_sock_, &ev);
    }

    // ---- client side -------------------------------------------------------

    uint32_t client_connect(const char* host, uint16_t port) override {
        if (conns_.empty() && connected_ids_.empty()) shutdown_by_transport_ = 0;
        UDTSOCKET s = make_socket();

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            UDT::close(s);
            throw std::runtime_error("inet_pton failed");
        }

        // UDT::connect is synchronous — blocks until handshake or error.
        if (UDT::connect(s, reinterpret_cast<sockaddr*>(&addr),
                         sizeof(addr)) == UDT::ERROR) {
            UDT::close(s);
            throw std::runtime_error("UDT::connect failed");
        }

        // Switch recv to non-blocking after connect.
        bool rcv_syn = false;
        UDT::setsockopt(s, 0, UDT_RCVSYN, &rcv_syn, sizeof(rcv_syn));

        uint32_t id = next_id_++;
        ConnState& conn = conns_[id];
        conn.sock = s;
        conn.id = id;
        sock_to_id_[s] = id;
        connected_ids_.insert(id);

        ensure_epoll();
        int ev = UDT_EPOLL_IN;
        UDT::epoll_add_usock(eid_, s, &ev);

        return id;
    }

    bool is_connected(uint32_t conn_id) override {
        return transport_connected(conn_id);
    }

    // ---- both sides --------------------------------------------------------

    // 送信は非ブロッキング。書き切れない分は per-conn の out_pending に積み、
    // poll() で再送する。旧実装は blocking send だったため、1 conn の
    // 送信バッファ詰まり(loss 由来の一時 stall)がプロセス内の全 conn の
    // tick/送受信を巻き込み、p99 RTT が秒単位に膨らんでいた。
    int send(uint32_t conn_id, const void* data, size_t len,
             bool /*reliable*/) override {
        if (len > max_payload_bytes(true)) return -1;
        if (!transport_connected(conn_id)) return -1;
        auto it = conns_.find(conn_id);
        if (it == conns_.end()) return -1;
        ConnState& conn = it->second;
        size_t frame_len = 4 + len;
        size_t limit = out_pending_byte_limit();
        size_t pending = conn.out_pending_size();
        if (pending > limit || frame_len > limit - pending) {
            return -1;
        }
        conn.compact_out_pending();

        // 4-byte LE length prefix
        uint32_t flen = static_cast<uint32_t>(len);
        uint8_t hdr[4] = {
            uint8_t(flen),
            uint8_t(flen >> 8),
            uint8_t(flen >> 16),
            uint8_t(flen >> 24)
        };
        conn.out_pending.insert(conn.out_pending.end(), hdr, hdr + 4);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        conn.out_pending.insert(conn.out_pending.end(), p, p + len);
        return flush_pending(conn_id) ? 0 : -1;
    }

    int recv(void* buf, size_t cap, size_t* out_len,
             uint32_t* out_conn_id) override {
        return inbox_.recv(buf, cap, out_len, out_conn_id);
    }

    void poll() override {
        if (eid_ == -1) return;

        std::set<UDTSOCKET> readfds;
        // timeout=0: non-blocking poll
        UDT::epoll_wait(eid_, &readfds, nullptr, 0);

        for (UDTSOCKET s : readfds) {
            if (s == listen_sock_) {
                accept_all();
            } else {
                auto it = sock_to_id_.find(s);
                if (it != sock_to_id_.end()) drain_conn(it->second);
            }
        }

        // 書き残しのある conn を再 flush(stall した conn は自分だけ遅れる)。
        std::vector<uint32_t> flush_ids;
        for (auto& [id, conn] : conns_) {
            if (conn.out_pending_size() > 0) flush_ids.push_back(id);
        }
        for (uint32_t id : flush_ids) {
            (void)flush_pending(id);
        }

        std::vector<uint32_t> connected_snapshot(
            connected_ids_.begin(), connected_ids_.end());
        for (uint32_t id : connected_snapshot) {
            (void)transport_connected(id);
        }
    }

    void close() override {
        if (eid_ != -1) {
            UDT::epoll_release(eid_);
            eid_ = -1;
        }
        for (auto& [id, conn] : conns_) {
            if (conn.sock != kInvalidSock) UDT::close(conn.sock);
        }
        conns_.clear();
        sock_to_id_.clear();
        connected_ids_.clear();
        is_server_ = false;
        if (listen_sock_ != kInvalidSock) {
            UDT::close(listen_sock_);
            listen_sock_ = kInvalidSock;
        }
    }

    const char* name() const override { return "udt4"; }
    // UDT4 is reliable-only; no unreliable datagram mode.
    bool supports(bool reliable) const override { return reliable; }
    size_t max_payload_bytes(bool /*reliable*/) const override { return 65536; }
    const char* flush_policy(bool reliable) const override {
        return reliable ? "nonblocking_stream_pending_queue" : "unsupported";
    }
    bool encryption_on() const override { return false; }
    // adapter が UDT 既定の CUDTCC(DAIMD) を loss 無反応の BenchCCC に
    // 置き換えている（audit §10）。
    const char* congestion_control() const override { return "none_benchccc"; }
    const char* thread_model() const override { return "internal_worker"; }
    rudp_bench::ConnectionStats connection_stats() const override {
        rudp_bench::ConnectionStats stats;
        stats.shutdown_by_transport = shutdown_by_transport_;
        return stats;
    }

 private:
    UDTSOCKET make_socket() {
        UDTSOCKET s = UDT::socket(AF_INET, SOCK_STREAM, 0);
        if (s == UDT::INVALID_SOCK)
            throw std::runtime_error("UDT::socket failed");
        // CC は connect/bind 前に設定する必要がある。listener に設定すると
        // accept されたソケットにも継承される。
        CCCFactory<BenchCCC> cc_factory;
        UDT::setsockopt(s, 0, UDT_CC, &cc_factory, sizeof(cc_factory));
        // 送信も非ブロッキング(書けない分は adapter 側 out_pending に積む)。
        bool snd_syn = false;
        UDT::setsockopt(s, 0, UDT_SNDSYN, &snd_syn, sizeof(snd_syn));
        return s;
    }

    void ensure_epoll() {
        if (eid_ == -1) eid_ = UDT::epoll_create();
    }

    // out_pending から書けるだけ書く。送信バッファ満杯(EASYNCSND)なら
    // 残りを保持して即 return(次の poll で再試行)。vector は連続領域なので
    // offset から直接 UDT::send に渡す(chunk への copy も前方 erase も不要)。
    bool flush_pending(uint32_t conn_id) {
        auto it = conns_.find(conn_id);
        if (it == conns_.end()) return false;
        ConnState& conn = it->second;
        while (conn.out_pending_size() > 0) {
            constexpr size_t kMaxChunk = 64 * 1024;
            size_t n = std::min(conn.out_pending_size(), kMaxChunk);
            const char* p = reinterpret_cast<const char*>(
                conn.out_pending.data() + conn.out_pending_offset);
            int sent = UDT::send(conn.sock, p, static_cast<int>(n), 0);
            if (sent == UDT::ERROR || sent <= 0) {
                int err = UDT::getlasterror().getErrorCode();
                if (err == CUDTException::EASYNCSND) break;
                mark_transport_shutdown(conn_id);
                return false;
            }
            conn.out_pending_offset += static_cast<size_t>(sent);
            if (static_cast<size_t>(sent) < n) break;
        }
        conn.compact_out_pending();
        return true;
    }

    void accept_all() {
        while (true) {
            sockaddr_in peer{};
            int plen = sizeof(peer);
            UDTSOCKET ns = UDT::accept(
                listen_sock_, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (ns == UDT::INVALID_SOCK) break;

            bool rcv_syn = false;
            UDT::setsockopt(ns, 0, UDT_RCVSYN, &rcv_syn, sizeof(rcv_syn));
            bool snd_syn = false;
            UDT::setsockopt(ns, 0, UDT_SNDSYN, &snd_syn, sizeof(snd_syn));

            uint32_t id = next_id_++;
            ConnState& conn = conns_[id];
            conn.sock = ns;
            conn.id = id;
            sock_to_id_[ns] = id;
            connected_ids_.insert(id);

            int ev = UDT_EPOLL_IN;
            UDT::epoll_add_usock(eid_, ns, &ev);
        }
    }

    void drain_conn(uint32_t id) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        ConnState& conn = it->second;

        char tmp[65536];
        while (true) {
            int n = UDT::recv(conn.sock, tmp, sizeof(tmp), 0);
            if (n == UDT::ERROR) {
                // EASYNCRCV: no data ready (non-blocking mode)
                int err = UDT::getlasterror().getErrorCode();
                if (err == CUDTException::EASYNCRCV)
                    break;
                mark_transport_shutdown(id);
                return;
            }
            if (n <= 0) break;
            conn.partial.insert(conn.partial.end(), tmp, tmp + n);
        }

        // Parse length-prefixed frames out of the accumulation buffer.
        while (conn.partial.size() - conn.partial_offset >= 4) {
            const uint8_t* frame = conn.partial.data() + conn.partial_offset;
            uint32_t msg_len =
                uint32_t(frame[0]) |
                (uint32_t(frame[1]) << 8) |
                (uint32_t(frame[2]) << 16) |
                (uint32_t(frame[3]) << 24);

            if (conn.partial.size() - conn.partial_offset < 4 + msg_len) break;

            inbox_.enqueue(id, frame + 4, msg_len);

            conn.partial_offset += 4 + msg_len;
        }

        if (conn.partial_offset == conn.partial.size()) {
            conn.partial.clear();
            conn.partial_offset = 0;
        } else if (conn.partial_offset > 4096 &&
                   conn.partial_offset * 2 >= conn.partial.size()) {
            conn.partial.erase(
                conn.partial.begin(),
                conn.partial.begin() + static_cast<std::ptrdiff_t>(conn.partial_offset));
            conn.partial_offset = 0;
        }
    }

    bool transport_connected(uint32_t conn_id) {
        if (connected_ids_.count(conn_id) == 0) return false;
        auto it = conns_.find(conn_id);
        if (it == conns_.end() || it->second.sock == kInvalidSock) {
            connected_ids_.erase(conn_id);
            return false;
        }

        UDTSTATUS state = UDT::getsockstate(it->second.sock);
        if (state == CONNECTED) return true;
        if (state == BROKEN || state == CLOSING || state == CLOSED ||
            state == NONEXIST) {
            mark_transport_shutdown(conn_id);
            return false;
        }
        return false;
    }

    void mark_transport_shutdown(uint32_t conn_id) {
        auto it = conns_.find(conn_id);
        if (it == conns_.end()) {
            connected_ids_.erase(conn_id);
            return;
        }
        ConnState conn = std::move(it->second);
        conns_.erase(it);
        if (connected_ids_.erase(conn_id) > 0) {
            ++shutdown_by_transport_;
        }
        if (conn.sock != kInvalidSock) {
            sock_to_id_.erase(conn.sock);
            if (eid_ != -1) UDT::epoll_remove_usock(eid_, conn.sock);
            UDT::close(conn.sock);
        }
    }

    bool is_server_ = false;
    UDTSOCKET listen_sock_ = kInvalidSock;
    int eid_ = -1;
    uint32_t next_id_ = 1;

    std::unordered_map<uint32_t, ConnState> conns_;
    std::unordered_map<UDTSOCKET, uint32_t> sock_to_id_;
    std::unordered_set<uint32_t> connected_ids_;
    rudp_bench::ReusableInboundQueue inbox_;
    uint32_t shutdown_by_transport_ = 0;
};

}  // namespace

namespace rudp_bench {
void register_udt4_adapter() {
    register_adapter("udt4", []() { return std::make_unique<Udt4Adapter>(); });
}
}  // namespace rudp_bench
