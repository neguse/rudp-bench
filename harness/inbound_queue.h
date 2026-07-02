#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace rudp_bench {

// §6.2: プロセス全体の inbound queue drop 累計。キューは各 adapter の内部に
// 埋まっていて Adapter インターフェースからは見えないため、runner が CSV の
// 診断列（inbox_dropped）に出せるようここでも集計する。adapter によっては
// 別スレッドから enqueue するので atomic（relaxed で十分: 単調カウンタを
// run 終了後に snapshot するだけ）。
inline std::atomic<uint64_t>& inbound_queue_dropped_total() {
  static std::atomic<uint64_t> total{0};
  return total;
}

class ReusableInboundQueue {
 public:
  // L14: optional bound. limit==0 (default) keeps the original unbounded
  // behaviour; a positive limit turns this into a ring — when full, the oldest
  // queued message is dropped (and counted) to make room. This caps harness
  // memory if the consumer ever falls behind, instead of growing without bound.
  void set_limit(size_t limit) { limit_ = limit; }
  uint64_t dropped() const { return dropped_; }

  void enqueue(uint32_t conn_id, const uint8_t* data, size_t len) {
    if (limit_ > 0 && ready_.size() >= limit_) {
      Message old = std::move(ready_.front());
      ready_.pop_front();
      recycle(std::move(old.data));
      ++dropped_;
      inbound_queue_dropped_total().fetch_add(1, std::memory_order_relaxed);
    }
    std::vector<uint8_t> buf;
    if (!free_.empty()) {
      buf = std::move(free_.back());
      free_.pop_back();
    }
    buf.resize(len);
    if (len > 0) std::memcpy(buf.data(), data, len);
    ready_.push_back(Message{conn_id, std::move(buf)});
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) {
    if (ready_.empty()) return 0;
    Message m = std::move(ready_.front());
    ready_.pop_front();
    *out_len = m.data.size();
    *out_conn_id = m.conn_id;
    if (m.data.size() > cap) {
      recycle(std::move(m.data));
      return -1;
    }
    if (!m.data.empty()) std::memcpy(buf, m.data.data(), m.data.size());
    recycle(std::move(m.data));
    return 1;
  }

  bool empty() const { return ready_.empty(); }
  size_t queued() const { return ready_.size(); }
  size_t free_buffers() const { return free_.size(); }

  void clear() {
    ready_.clear();
    free_.clear();
  }

 private:
  struct Message {
    uint32_t conn_id;
    std::vector<uint8_t> data;
  };

  void recycle(std::vector<uint8_t>&& buf) {
    buf.clear();
    free_.push_back(std::move(buf));
  }

  std::deque<Message> ready_;
  std::vector<std::vector<uint8_t>> free_;
  size_t limit_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace rudp_bench
