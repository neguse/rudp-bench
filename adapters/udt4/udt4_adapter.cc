#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

// UDT4 SDK 4.11 — vendored via FetchContent in adapters/udt4/CMakeLists.txt
#include <udt.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstring>
#include <deque>
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
// Unreliable mode is NOT supported by UDT4.  supports(false) returns false;
// the harness emits an 'na' row for --reliable=u scenarios.
//
// One UDTSOCKET per connection.  Server accept() loop runs inside poll().
// Recv is made non-blocking via UDT_RCVSYN=false; send stays blocking so
// that the send_all helper can guarantee delivery without busy-looping.
//
// Fork selection: loonycyborg/UDT-fixed → HTTP 404; eric-yhc/udt4 → HTTP 404;
// git HTTPS clone also unavailable in this environment.  System apt package
// libudt-dev 4.11+dfsg1 chosen (vendor approach option from the spec).
// ============================================================

namespace {

static constexpr UDTSOCKET kInvalidSock = -1;

void ensure_udt_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        UDT::startup();
        std::atexit([]() { UDT::cleanup(); });
    });
}

struct ConnState {
    UDTSOCKET sock = kInvalidSock;
    uint32_t id = 0;
    std::vector<uint8_t> partial;
    size_t partial_offset = 0;
};

class Udt4Adapter : public rudp_bench::Adapter {
 public:
    Udt4Adapter() { ensure_udt_init(); }
    ~Udt4Adapter() override { close(); }

    // ---- server side -------------------------------------------------------

    void server_listen(uint16_t port) override {
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
        return connected_ids_.count(conn_id) > 0;
    }

    // ---- both sides --------------------------------------------------------

    int send(uint32_t conn_id, const void* data, size_t len,
             bool /*reliable*/) override {
        auto it = conns_.find(conn_id);
        if (it == conns_.end()) return -1;
        UDTSOCKET s = it->second.sock;

        // 4-byte LE length prefix
        uint32_t flen = static_cast<uint32_t>(len);
        uint8_t hdr[4] = {
            uint8_t(flen),
            uint8_t(flen >> 8),
            uint8_t(flen >> 16),
            uint8_t(flen >> 24)
        };
        if (send_all(s, hdr, 4) < 0) return -1;
        if (send_all(s, static_cast<const uint8_t*>(data), len) < 0) return -1;
        return 0;
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
        return reliable ? "blocking_stream" : "unsupported";
    }
    bool encryption_on() const override { return false; }

 private:
    UDTSOCKET make_socket() {
        UDTSOCKET s = UDT::socket(AF_INET, SOCK_STREAM, 0);
        if (s == UDT::INVALID_SOCK)
            throw std::runtime_error("UDT::socket failed");
        return s;
    }

    void ensure_epoll() {
        if (eid_ == -1) eid_ = UDT::epoll_create();
    }

    int send_all(UDTSOCKET s, const void* data, size_t len) {
        const char* p = static_cast<const char*>(data);
        size_t remaining = len;
        while (remaining > 0) {
            int n = UDT::send(s, p, static_cast<int>(remaining), 0);
            if (n == UDT::ERROR) return -1;
            p += n;
            remaining -= static_cast<size_t>(n);
        }
        return 0;
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
                if (UDT::getlasterror().getErrorCode() ==
                    CUDTException::EASYNCRCV)
                    break;
                break;  // other error — stop draining this conn
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

    bool is_server_ = false;
    UDTSOCKET listen_sock_ = kInvalidSock;
    int eid_ = -1;
    uint32_t next_id_ = 1;

    std::unordered_map<uint32_t, ConnState> conns_;
    std::unordered_map<UDTSOCKET, uint32_t> sock_to_id_;
    std::unordered_set<uint32_t> connected_ids_;
    rudp_bench::ReusableInboundQueue inbox_;
};

}  // namespace

namespace rudp_bench {
void register_udt4_adapter() {
    register_adapter("udt4", []() { return std::make_unique<Udt4Adapter>(); });
}
}  // namespace rudp_bench
