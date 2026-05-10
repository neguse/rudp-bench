#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace rudp_bench {

class ReusableInboundQueue {
 public:
  void enqueue(uint32_t conn_id, const uint8_t* data, size_t len) {
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
};

}  // namespace rudp_bench
