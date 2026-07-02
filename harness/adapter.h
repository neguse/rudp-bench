#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace rudp_bench {

// adapter が観測した接続イベントの累計。観測できない lib は default {0,0,0}。
struct ConnectionStats {
  uint32_t connected_peak = 0;        // 同時 connected の最大数(成功した最大数)
  uint32_t shutdown_by_transport = 0; // bench 中に transport 層で切られた数
  uint32_t shutdown_by_peer = 0;      // peer から close 通告された数
};

// 失敗時 -1。recv で out_conn_id にメッセージ送信元の conn_id が入る。
struct Adapter {
  virtual ~Adapter() = default;

  // Optional hint: the run will use about `n` connections (per role/process).
  // Adapters that pre-size per-peer structures (e.g. enet's host peerCount)
  // can use it to avoid a fixed cost proportional to a hardcoded maximum.
  // Called once before server_listen / the client_connect loop. Default no-op.
  virtual void hint_connections(uint32_t /*n*/) {}

  // server-side: バインドして listen 開始。失敗時は abort。
  virtual void server_listen(uint16_t port) = 0;

  // client-side: 接続要求を発行し handle を返す。非同期 lib 用に is_connected で確認。
  virtual uint32_t client_connect(const char* host, uint16_t port) = 0;
  virtual bool is_connected(uint32_t conn_id) = 0;

  // both sides
  // send: 成功時 0、リソース不足等で送信不可なら -1
  virtual int send(uint32_t conn_id, const void* data, size_t len, bool reliable) = 0;
  // Server-side broadcast helper. Default preserves the one-send-per-target
  // behavior; high-fanout adapters can override to batch queueing/copies.
  virtual size_t send_many(const uint32_t* conn_ids, size_t count,
                           const void* data, size_t len, bool reliable) {
    size_t accepted = 0;
    for (size_t i = 0; i < count; ++i) {
      if (send(conn_ids[i], data, len, reliable) == 0) ++accepted;
    }
    return accepted;
  }
  // recv: メッセージ取得時 1、なければ 0、エラー -1。out_* に書き込み。
  virtual int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) = 0;

  virtual void poll() = 0;
  virtual void close() = 0;

  virtual const char* name() const = 0;
  virtual bool supports(bool reliable) const = 0;
  virtual size_t max_payload_bytes(bool /*reliable*/) const {
    return std::numeric_limits<size_t>::max();
  }
  virtual uint32_t max_connections() const {
    return std::numeric_limits<uint32_t>::max();
  }
  virtual const char* flush_policy(bool /*reliable*/) const { return "immediate"; }
  virtual bool encryption_on() const = 0;
  // ベンチ公平性メタデータ（docs/improvements.md §3.2-3.3）。
  // 実効の輻輳制御アルゴリズム。adapter がライブラリ既定を上書きしている場合は
  // 上書き後の値を返す（例: kcp は nocwnd=1 なら "none_nocwnd"）。
  // CC 無効群と BBR 群を結果表で分離して読むための列になる。
  virtual const char* congestion_control() const { return "unknown"; }
  // スレッドモデル。"single" = harness スレッドのみ / "internal_worker" =
  // ライブラリ内部スレッドが送受信を担う / "adapter_worker" = adapter が
  // ワーカースレッドを生成。CPU% とスループットの解釈（dev-notes §5.2）に使う。
  virtual const char* thread_model() const { return "unknown"; }
  // 観測可能な adapter は connection event を記録して返す。デフォルトは全 0。
  virtual ConnectionStats connection_stats() const { return {}; }
};

}  // namespace rudp_bench
