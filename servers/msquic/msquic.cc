// msquic の層。LT は QUIC datagram(RFC 9221)、MD は接続ごとに 1 本の
// 双方向 stream に長さ 4 byte をつけて載せる。設定は既定のまま、使い方に要る
// 2 つ(datagram の受信、相手が開く stream の数)だけ有効にする。
//
// msquic のイベントは msquic のスレッドで届くので、ここで溜めて service() で渡す。
#include "bk_sut.h"

#include <msquic.h>

#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

const QUIC_API_TABLE *MsQuic = nullptr;

uint8_t kAlpnBytes[] = {'r', 'u', 'd', 'p', '-', 'b', 'e', 'n', 'c', 'h'};
const QUIC_BUFFER kAlpn = {sizeof(kAlpnBytes), kAlpnBytes};

enum class EventType { Connect, Receive, Disconnect };

struct Event {
  EventType type;
  int conn;
  std::vector<uint8_t> data;
};

struct quic_t;

struct Conn {
  quic_t *q = nullptr;
  int index = 0;
  HQUIC conn = nullptr;
  HQUIC stream = nullptr;
  std::vector<uint8_t> rx;  // stream の受信途中の bytes
};

// 送信が終わるまで msquic に渡したバッファを保つ
struct SendBuf {
  QUIC_BUFFER buffer;
  uint8_t data[1];
};

struct quic_t {
  bk_sut *sut = nullptr;
  bool server = false;
  HQUIC registration = nullptr;
  HQUIC configuration = nullptr;
  HQUIC listener = nullptr;
  std::vector<Conn> conns;
  int accepted = 0;  // server: 番号を振った接続の数
  std::mutex mu;     // conns の handle と events を守る
  std::condition_variable cv;
  std::vector<Event> events;
  std::string cert;
  std::string key;
};

void post(quic_t *q, EventType type, int conn, const uint8_t *data = nullptr,
          size_t len = 0) {
  {
    std::lock_guard<std::mutex> lock(q->mu);
    q->events.push_back(Event{type, conn, std::vector<uint8_t>(data, data + len)});
  }
  q->cv.notify_one();
}

SendBuf *send_buf(size_t len) {
  auto *b = static_cast<SendBuf *>(std::malloc(sizeof(SendBuf) + len));
  if (b != nullptr) {
    b->buffer.Buffer = b->data;
    b->buffer.Length = static_cast<uint32_t>(len);
  }
  return b;
}

QUIC_STATUS QUIC_API stream_cb(HQUIC stream, void *ctx, QUIC_STREAM_EVENT *ev) {
  Conn *c = static_cast<Conn *>(ctx);
  switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
      for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
        const QUIC_BUFFER &b = ev->RECEIVE.Buffers[i];
        c->rx.insert(c->rx.end(), b.Buffer, b.Buffer + b.Length);
      }
      for (;;) {
        uint32_t len = 0;
        if (c->rx.size() < sizeof(len)) {
          break;
        }
        std::memcpy(&len, c->rx.data(), sizeof(len));
        len = ntohl(len);
        if (c->rx.size() < sizeof(len) + len) {
          break;
        }
        post(c->q, EventType::Receive, c->index, c->rx.data() + sizeof(len), len);
        c->rx.erase(c->rx.begin(), c->rx.begin() + sizeof(len) + len);
      }
      break;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
      std::free(ev->SEND_COMPLETE.ClientContext);
      break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
      {
        std::lock_guard<std::mutex> lock(c->q->mu);
        c->stream = nullptr;
      }
      MsQuic->StreamClose(stream);
      break;
    }
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API conn_cb(HQUIC conn, void *ctx, QUIC_CONNECTION_EVENT *ev) {
  Conn *c = static_cast<Conn *>(ctx);
  quic_t *q = c->q;
  switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
      if (!q->server) {
        HQUIC stream = nullptr;
        if (QUIC_FAILED(MsQuic->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE,
                                           stream_cb, c, &stream)) ||
            QUIC_FAILED(MsQuic->StreamStart(stream,
                                            QUIC_STREAM_START_FLAG_IMMEDIATE))) {
          post(q, EventType::Disconnect, c->index);
          break;
        }
        std::lock_guard<std::mutex> lock(q->mu);
        c->stream = stream;
      }
      post(q, EventType::Connect, c->index);
      break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
      HQUIC stream = ev->PEER_STREAM_STARTED.Stream;
      MsQuic->SetCallbackHandler(stream, reinterpret_cast<void *>(stream_cb), c);
      std::lock_guard<std::mutex> lock(q->mu);
      c->stream = stream;
      break;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
      const QUIC_BUFFER *b = ev->DATAGRAM_RECEIVED.Buffer;
      post(q, EventType::Receive, c->index, b->Buffer, b->Length);
      break;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
      if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(
              ev->DATAGRAM_SEND_STATE_CHANGED.State)) {
        std::free(ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
      }
      break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      std::fprintf(stderr, "msquic: conn %d shutdown by transport status=0x%x\n",
                   c->index,
                   static_cast<unsigned>(
                       ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
      break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
      post(q, EventType::Disconnect, c->index);
      break;
    default:
      break;
  }
  return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API listener_cb(HQUIC, void *ctx, QUIC_LISTENER_EVENT *ev) {
  quic_t *q = static_cast<quic_t *>(ctx);
  if (ev->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
    return QUIC_STATUS_SUCCESS;
  }
  std::unique_lock<std::mutex> lock(q->mu);
  if (q->accepted == static_cast<int>(q->conns.size())) {
    return QUIC_STATUS_CONNECTION_REFUSED;
  }
  Conn *c = &q->conns[static_cast<size_t>(q->accepted++)];
  c->conn = ev->NEW_CONNECTION.Connection;
  lock.unlock();
  MsQuic->SetCallbackHandler(c->conn, reinterpret_cast<void *>(conn_cb), c);
  return MsQuic->ConnectionSetConfiguration(c->conn, q->configuration);
}

void quic_close(void *t) {
  quic_t *q = static_cast<quic_t *>(t);
  if (q->listener != nullptr) {
    MsQuic->ListenerClose(q->listener);
  }
  for (Conn &c : q->conns) {
    if (c.conn != nullptr) {
      MsQuic->ConnectionShutdown(c.conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
      MsQuic->ConnectionClose(c.conn);
    }
  }
  if (q->configuration != nullptr) {
    MsQuic->ConfigurationClose(q->configuration);
  }
  if (q->registration != nullptr) {
    MsQuic->RegistrationClose(q->registration);
  }
  if (!q->cert.empty()) {
    unlink(q->cert.c_str());
    unlink(q->key.c_str());
  }
  delete q;
  MsQuicClose(MsQuic);
  MsQuic = nullptr;
}

quic_t *create(bk_sut *sut, bool server, int conns) {
  if (QUIC_FAILED(MsQuicOpen2(&MsQuic))) {
    return nullptr;
  }
  quic_t *q = new quic_t;
  q->sut = sut;
  q->server = server;
  q->conns.resize(static_cast<size_t>(conns));
  for (int i = 0; i < conns; ++i) {
    q->conns[i].q = q;
    q->conns[i].index = i;
  }
  const QUIC_REGISTRATION_CONFIG reg = {"rudp-bench",
                                        QUIC_EXECUTION_PROFILE_LOW_LATENCY};
  QUIC_SETTINGS settings{};
  settings.DatagramReceiveEnabled = TRUE;
  settings.IsSet.DatagramReceiveEnabled = TRUE;
  if (server) {
    settings.PeerBidiStreamCount = 1;
    settings.IsSet.PeerBidiStreamCount = TRUE;
  }
  QUIC_CREDENTIAL_CONFIG cred{};
  QUIC_CERTIFICATE_FILE cert{};
  if (server) {
    // 自己署名の証明書をその場で作る(client は検証しない)
    const std::string base = "/tmp/rudp-bench-msquic-" + std::to_string(getpid());
    q->cert = base + "-cert.pem";
    q->key = base + "-key.pem";
    const std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -nodes -days 1 -keyout " + q->key +
        " -out " + q->cert + " -subj /CN=rudp-bench 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) {
      quic_close(q);
      return nullptr;
    }
    cert.CertificateFile = q->cert.c_str();
    cert.PrivateKeyFile = q->key.c_str();
    cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred.CertificateFile = &cert;
  } else {
    cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                 QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  }
  if (QUIC_FAILED(MsQuic->RegistrationOpen(&reg, &q->registration)) ||
      QUIC_FAILED(MsQuic->ConfigurationOpen(q->registration, &kAlpn, 1,
                                            &settings, sizeof(settings),
                                            nullptr, &q->configuration)) ||
      QUIC_FAILED(MsQuic->ConfigurationLoadCredential(q->configuration, &cred))) {
    quic_close(q);
    return nullptr;
  }
  return q;
}

void *quic_listen(bk_sut *sut, uint16_t port, int max_conns) {
  quic_t *q = create(sut, true, max_conns);
  if (q == nullptr) {
    return nullptr;
  }
  QUIC_ADDR addr{};
  QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
  QuicAddrSetPort(&addr, port);
  if (QUIC_FAILED(MsQuic->ListenerOpen(q->registration, listener_cb, q,
                                       &q->listener)) ||
      QUIC_FAILED(MsQuic->ListenerStart(q->listener, &kAlpn, 1, &addr))) {
    quic_close(q);
    return nullptr;
  }
  return q;
}

void *quic_connect(bk_sut *sut, const char *host, uint16_t port, int conns) {
  quic_t *q = create(sut, false, conns);
  if (q == nullptr) {
    return nullptr;
  }
  for (Conn &c : q->conns) {
    if (QUIC_FAILED(MsQuic->ConnectionOpen(q->registration, conn_cb, &c,
                                           &c.conn)) ||
        QUIC_FAILED(MsQuic->ConnectionStart(c.conn, q->configuration,
                                            QUIC_ADDRESS_FAMILY_UNSPEC, host,
                                            port))) {
      quic_close(q);
      return nullptr;
    }
  }
  return q;
}

int quic_send(void *t, int conn, bool must_deliver, const void *data,
              size_t len) {
  quic_t *q = static_cast<quic_t *>(t);
  const uint32_t prefix = must_deliver ? sizeof(uint32_t) : 0;
  SendBuf *b = send_buf(prefix + len);
  if (b == nullptr) {
    return -1;
  }
  if (must_deliver) {
    const uint32_t nlen = htonl(static_cast<uint32_t>(len));
    std::memcpy(b->data, &nlen, sizeof(nlen));
  }
  std::memcpy(b->data + prefix, data, len);
  QUIC_STATUS status = QUIC_STATUS_INVALID_STATE;
  {
    std::lock_guard<std::mutex> lock(q->mu);
    Conn &c = q->conns[static_cast<size_t>(conn)];
    if (!must_deliver && c.conn != nullptr) {
      status = MsQuic->DatagramSend(c.conn, &b->buffer, 1, QUIC_SEND_FLAG_NONE, b);
    } else if (must_deliver && c.stream != nullptr) {
      status = MsQuic->StreamSend(c.stream, &b->buffer, 1, QUIC_SEND_FLAG_NONE, b);
    }
  }
  if (QUIC_FAILED(status)) {
    std::free(b);
    return -1;
  }
  return 0;
}

int quic_service(void *t, uint64_t timeout_ns) {
  quic_t *q = static_cast<quic_t *>(t);
  std::vector<Event> events;
  {
    std::unique_lock<std::mutex> lock(q->mu);
    q->cv.wait_for(lock, std::chrono::nanoseconds(timeout_ns),
                   [q] { return !q->events.empty(); });
    events.swap(q->events);
  }
  for (const Event &e : events) {
    switch (e.type) {
      case EventType::Connect:
        bk_sut_on_connect(q->sut, e.conn);
        break;
      case EventType::Receive:
        bk_sut_on_receive(q->sut, e.conn, e.data.data(), e.data.size());
        break;
      case EventType::Disconnect:
        bk_sut_on_disconnect(q->sut, e.conn);
        break;
    }
  }
  return 0;
}

const bk_transport transport = [] {
  bk_transport t{};
  t.name = "msquic";
  t.describe =
      "\"class_mapping\":{"
      "\"loss_tolerant\":{\"primitive\":\"quic datagram\","
      "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
      "\"realization\":\"native\"},"
      "\"must_deliver\":{\"primitive\":\"quic stream\","
      "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
      "\"realization\":\"native\"}},"
      "\"coalescing\":\"none\",\"cc_algo\":\"cubic (default)\","
      "\"thread_model\":\"internal_worker\",\"encryption\":true,"
      "\"tuning\":["
      "{\"knob\":\"DatagramReceiveEnabled\",\"value\":\"true\","
      "\"upstream_ref\":\"docs/Settings.md\"},"
      "{\"knob\":\"PeerBidiStreamCount\",\"value\":\"1 (server)\","
      "\"upstream_ref\":\"docs/Settings.md\"}]";
  t.max_payload = 1000;
  t.listen = quic_listen;
  t.connect = quic_connect;
  t.send = quic_send;
  t.service = quic_service;
  t.close = quic_close;
  return t;
}();

}  // namespace

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
