#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace rudp_bench {

// 失敗時 -1。recv で out_conn_id にメッセージ送信元の conn_id が入る。
struct Adapter {
  virtual ~Adapter() = default;

  // server-side: バインドして listen 開始。失敗時は abort。
  virtual void server_listen(uint16_t port) = 0;

  // client-side: 接続要求を発行し handle を返す。非同期 lib 用に is_connected で確認。
  virtual uint32_t client_connect(const char* host, uint16_t port) = 0;
  virtual bool is_connected(uint32_t conn_id) = 0;

  // both sides
  // send: 成功時 0、リソース不足等で送信不可なら -1
  virtual int send(uint32_t conn_id, const void* data, size_t len, bool reliable) = 0;
  // recv: メッセージ取得時 1、なければ 0、エラー -1。out_* に書き込み。
  virtual int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) = 0;

  virtual void poll() = 0;
  virtual void close() = 0;

  virtual const char* name() const = 0;
  virtual bool supports(bool reliable) const = 0;
  virtual size_t max_payload_bytes(bool /*reliable*/) const {
    return std::numeric_limits<size_t>::max();
  }
  virtual bool encryption_on() const = 0;
};

}  // namespace rudp_bench
