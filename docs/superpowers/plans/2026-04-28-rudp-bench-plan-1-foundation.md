# RUDP Bench Plan 1: Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** RUDP ベンチマーク・ハーネスの土台を構築し、`raw_udp` と `mini_rudp` の2ベースラインだけで Phase 1 を end-to-end で走らせられる状態にする。

**Architecture:** 単一 C++ バイナリ `rudp-bench` が `--library` フラグで adapter を切り替える。adapter は抽象 IF を実装した静的登録クラス。シナリオパラメータは CLI フラグで渡し、結果は CSV 1行を `--out` に書く。`scripts/run_phase1.sh` が tc qdisc でロス注入しつつ全シナリオをループ起動し、`results/phase1.csv` に集約する。Python 側で表/グラフを生成。

**Tech Stack:**
- C++17 / CMake (≥3.20) / POSIX sockets
- GoogleTest (FetchContent で取得)
- bash + iproute2 (`tc`) for runner script
- Python 3 + pandas + matplotlib for post-processing

---

## File Structure

```
rudp-bench/
├── CMakeLists.txt                    # トップ
├── .gitignore
├── README.md
├── harness/
│   ├── CMakeLists.txt
│   ├── adapter.h                     # 抽象 IF + 登録ヘルパ
│   ├── adapter_registry.h/cc         # ライブラリ名 → factory のレジストリ
│   ├── scenario.h/cc                 # CLI フラグ → ScenarioConfig 構造体
│   ├── metrics.h/cc                  # latency histogram + throughput counter + delivery tracker
│   ├── csv_writer.h/cc               # CSV 1行出力
│   ├── proc_sampler.h/cc             # getrusage + /proc/self/status の CPU/RSS 採取
│   ├── runner.h/cc                   # サーバ/クライアントの送受信ループ
│   └── main.cc                       # CLI エントリ
├── adapters/
│   ├── raw_udp/
│   │   ├── CMakeLists.txt
│   │   └── raw_udp_adapter.cc
│   └── mini_rudp/
│       ├── CMakeLists.txt
│       └── mini_rudp_adapter.cc
├── tests/
│   ├── CMakeLists.txt
│   ├── test_metrics.cc
│   ├── test_csv_writer.cc
│   ├── test_scenario.cc
│   ├── test_proc_sampler.cc
│   ├── test_raw_udp_smoke.cc
│   └── test_mini_rudp_smoke.cc
├── scripts/
│   ├── run_phase1.sh                 # tc 注入 + シナリオ全ループ
│   ├── set_loss.sh                   # tc qdisc ヘルパ(sudo 必要)
│   └── plot.py                       # phase1_table.md 生成 + Phase 2 用プロット雛形
├── results/                          # gitignore
└── docs/
```

各ファイルの責務:
- `adapter.h`: 全 adapter が実装する IF。capability query 含む
- `adapter_registry.*`: 静的登録パターン。各 adapter cc が `REGISTER_ADAPTER(name, factory)` で名前→factoryを登録
- `scenario.*`: CLI 引数を `ScenarioConfig` に詰める。`--reliable=r/u`, `--size`, `--conns`, `--rate`, `--duration`, `--warmup`, `--loss` 等を持つ(loss はメタデータ、tc は外側で注入)
- `metrics.*`: 1conn あたりの送受信状態 + RTT サンプル配列(percentile は終了時にソートして算出)
- `csv_writer.*`: CSV ヘッダと 1行の整形
- `proc_sampler.*`: `getrusage(RUSAGE_SELF)` の CPU 時間と `/proc/self/status` の `VmRSS` を beg/end で取って差分・最大を取る
- `runner.*`: サーバはエコー、クライアントは N conns ぶん非同期に送信(レート制御)+受信(RTT 算出)、warmup の先頭時間は計測除外
- `main.cc`: 引数解釈 → adapter 生成 → role に応じた loop 呼び出し → CSV 出力

---

## Conventions

- **TDD**: 各タスクで失敗するテストを先に書き、最小実装で通し、リファクタする
- **コミット粒度**: タスク末尾で 1 コミット
- **Build**: `cmake -S . -B build && cmake --build build -j`
- **Test**: `ctest --test-dir build --output-on-failure`
- **コード規約**: C++17、`#pragma once`、ヘッダは前方宣言を優先、`snake_case` for functions / `PascalCase` for types
- **失敗テストの確認**: 全タスクの「test fail」ステップでは `ctest -R <name> --output-on-failure` で実行し、エラーメッセージが想定どおりであることを確認

---

## Task 1: Repo skeleton + top-level CMake

**Files:**
- Create: `CMakeLists.txt`
- Create: `.gitignore`
- Create: `README.md`
- Create: `harness/CMakeLists.txt` (placeholder)
- Create: `tests/CMakeLists.txt` (placeholder)

- [ ] **Step 1: Write `.gitignore`**

```
build/
results/
.cache/
compile_commands.json
*.swp
__pycache__/
```

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(rudp-bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_subdirectory(harness)
add_subdirectory(adapters/raw_udp)
add_subdirectory(adapters/mini_rudp)
add_subdirectory(tests)
```

- [ ] **Step 3: Write `harness/CMakeLists.txt` placeholder**

```cmake
# placeholder; sources added in later tasks
add_library(rudp_bench_harness INTERFACE)
target_include_directories(rudp_bench_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 4: Write `tests/CMakeLists.txt` placeholder**

```cmake
# tests added in later tasks
```

- [ ] **Step 5: Write `adapters/raw_udp/CMakeLists.txt` and `adapters/mini_rudp/CMakeLists.txt` placeholders**

両方とも:
```cmake
# placeholder; sources added in later tasks
```

- [ ] **Step 6: Write minimal README.md**

```markdown
# rudp-bench

Cross-library benchmark harness for reliable UDP / RUDP / QUIC implementations.
See `docs/superpowers/specs/2026-04-28-rudp-bench-design.md`.

## Build

cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

- [ ] **Step 7: Verify configure succeeds**

Run: `cmake -S . -B build`
Expected: configure完了、エラーなし(ターゲットなしの警告は出る場合あり、許容)

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt .gitignore README.md harness/CMakeLists.txt \
  tests/CMakeLists.txt adapters/raw_udp/CMakeLists.txt adapters/mini_rudp/CMakeLists.txt
git commit -m "chore: scaffold cmake build skeleton"
```

---

## Task 2: Adapter abstract interface

**Files:**
- Create: `harness/adapter.h`

- [ ] **Step 1: Write `harness/adapter.h`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
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
  virtual bool encryption_on() const = 0;
};

}  // namespace rudp_bench
```

- [ ] **Step 2: Verify compile**

Run: `cmake --build build -j`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add harness/adapter.h
git commit -m "feat(harness): add Adapter abstract interface"
```

---

## Task 3: Adapter registry

**Files:**
- Create: `harness/adapter_registry.h`
- Create: `harness/adapter_registry.cc`
- Create: `tests/test_adapter_registry.cc`
- Modify: `harness/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write `tests/test_adapter_registry.cc` (failing test)**

```cpp
#include <gtest/gtest.h>
#include "harness/adapter.h"
#include "harness/adapter_registry.h"

namespace {

class FakeAdapter : public rudp_bench::Adapter {
 public:
  void server_listen(uint16_t) override {}
  uint32_t client_connect(const char*, uint16_t) override { return 0; }
  bool is_connected(uint32_t) override { return true; }
  int send(uint32_t, const void*, size_t, bool) override { return 0; }
  int recv(void*, size_t, size_t*, uint32_t*) override { return 0; }
  void poll() override {}
  void close() override {}
  const char* name() const override { return "fake"; }
  bool supports(bool) const override { return true; }
  bool encryption_on() const override { return false; }
};

TEST(AdapterRegistry, RegistersAndCreates) {
  rudp_bench::register_adapter("fake", []() { return std::make_unique<FakeAdapter>(); });
  auto a = rudp_bench::create_adapter("fake");
  ASSERT_NE(a, nullptr);
  EXPECT_STREQ(a->name(), "fake");
}

TEST(AdapterRegistry, UnknownReturnsNull) {
  EXPECT_EQ(rudp_bench::create_adapter("does-not-exist"), nullptr);
}

}  // namespace
```

- [ ] **Step 2: Update `tests/CMakeLists.txt`**

```cmake
add_executable(test_adapter_registry test_adapter_registry.cc)
target_link_libraries(test_adapter_registry PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_adapter_registry COMMAND test_adapter_registry)
```

- [ ] **Step 3: Run test, verify FAIL**

Run: `cmake --build build -j 2>&1 | tail -20`
Expected: 未定義 `register_adapter` / `create_adapter` でコンパイル失敗

- [ ] **Step 4: Write `harness/adapter_registry.h`**

```cpp
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "harness/adapter.h"

namespace rudp_bench {

using AdapterFactory = std::function<std::unique_ptr<Adapter>()>;

void register_adapter(const std::string& name, AdapterFactory factory);
std::unique_ptr<Adapter> create_adapter(const std::string& name);

}  // namespace rudp_bench
```

- [ ] **Step 5: Write `harness/adapter_registry.cc`**

```cpp
#include "harness/adapter_registry.h"

#include <unordered_map>

namespace rudp_bench {
namespace {
std::unordered_map<std::string, AdapterFactory>& registry() {
  static std::unordered_map<std::string, AdapterFactory> r;
  return r;
}
}  // namespace

void register_adapter(const std::string& name, AdapterFactory factory) {
  registry()[name] = std::move(factory);
}

std::unique_ptr<Adapter> create_adapter(const std::string& name) {
  auto it = registry().find(name);
  if (it == registry().end()) return nullptr;
  return it->second();
}

}  // namespace rudp_bench
```

- [ ] **Step 6: Update `harness/CMakeLists.txt`**

```cmake
add_library(rudp_bench_harness STATIC
  adapter_registry.cc
)
target_include_directories(rudp_bench_harness PUBLIC ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 7: Build & run test, verify PASS**

Run: `cmake --build build -j && ctest --test-dir build -R test_adapter_registry --output-on-failure`
Expected: 2 tests PASS

- [ ] **Step 8: Commit**

```bash
git add harness/adapter_registry.h harness/adapter_registry.cc \
  harness/CMakeLists.txt tests/test_adapter_registry.cc tests/CMakeLists.txt
git commit -m "feat(harness): add adapter registry with factory pattern"
```

---

## Task 4: Scenario config

**Files:**
- Create: `harness/scenario.h`
- Create: `harness/scenario.cc`
- Create: `tests/test_scenario.cc`
- Modify: `harness/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write `tests/test_scenario.cc` (failing test)**

```cpp
#include <gtest/gtest.h>
#include "harness/scenario.h"

TEST(Scenario, ParsesAllFlags) {
  const char* argv[] = {
      "rudp-bench",
      "--library=raw_udp", "--role=client",
      "--host=127.0.0.1", "--port=9000",
      "--reliable=u", "--size=64", "--conns=4", "--rate=100",
      "--duration=30", "--warmup=2", "--loss=0",
      "--out=/tmp/out.csv",
  };
  int argc = sizeof(argv) / sizeof(argv[0]);
  auto cfg = rudp_bench::parse_scenario(argc, argv);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->library, "raw_udp");
  EXPECT_EQ(cfg->role, rudp_bench::Role::Client);
  EXPECT_EQ(cfg->host, "127.0.0.1");
  EXPECT_EQ(cfg->port, 9000);
  EXPECT_EQ(cfg->reliable, rudp_bench::Reliability::Unreliable);
  EXPECT_EQ(cfg->size_bytes, 64u);
  EXPECT_EQ(cfg->conns, 4u);
  EXPECT_EQ(cfg->rate_per_conn, 100u);
  EXPECT_EQ(cfg->duration_s, 30u);
  EXPECT_EQ(cfg->warmup_s, 2u);
  EXPECT_DOUBLE_EQ(cfg->loss_pct, 0.0);
  EXPECT_EQ(cfg->out_path, "/tmp/out.csv");
}

TEST(Scenario, RejectsUnknownFlag) {
  const char* argv[] = {"rudp-bench", "--bogus=1"};
  EXPECT_FALSE(rudp_bench::parse_scenario(2, argv).has_value());
}

TEST(Scenario, RoleServerDoesNotRequireClientFlags) {
  const char* argv[] = {
      "rudp-bench", "--library=raw_udp", "--role=server",
      "--port=9000", "--duration=30", "--out=/tmp/s.csv",
  };
  int argc = sizeof(argv) / sizeof(argv[0]);
  auto cfg = rudp_bench::parse_scenario(argc, argv);
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->role, rudp_bench::Role::Server);
}
```

- [ ] **Step 2: Update `tests/CMakeLists.txt`**

```cmake
add_executable(test_scenario test_scenario.cc)
target_link_libraries(test_scenario PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_scenario COMMAND test_scenario)
```

- [ ] **Step 3: Run, verify FAIL**

Run: `cmake --build build -j 2>&1 | tail -10`
Expected: コンパイル失敗(parse_scenario 未定義)

- [ ] **Step 4: Write `harness/scenario.h`**

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace rudp_bench {

enum class Role { Server, Client };
enum class Reliability { Reliable, Unreliable, NotApplicable };

struct ScenarioConfig {
  std::string library;
  Role role = Role::Client;
  std::string host = "127.0.0.1";
  uint16_t port = 9000;
  Reliability reliable = Reliability::Unreliable;
  uint32_t size_bytes = 64;
  uint32_t conns = 1;
  uint32_t rate_per_conn = 0;     // 0 = unbounded
  uint32_t duration_s = 30;
  uint32_t warmup_s = 2;
  double loss_pct = 0.0;          // メタデータ(tc は外側で設定済み前提)
  std::string out_path;
};

std::optional<ScenarioConfig> parse_scenario(int argc, const char* argv[]);

}  // namespace rudp_bench
```

- [ ] **Step 5: Write `harness/scenario.cc`**

```cpp
#include "harness/scenario.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace rudp_bench {
namespace {

bool starts_with(const char* s, const char* p) {
  return std::strncmp(s, p, std::strlen(p)) == 0;
}

const char* value(const char* s, const char* p) {
  return s + std::strlen(p);
}

}  // namespace

std::optional<ScenarioConfig> parse_scenario(int argc, const char* argv[]) {
  ScenarioConfig c;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (starts_with(a, "--library=")) c.library = value(a, "--library=");
    else if (starts_with(a, "--role=")) {
      const char* v = value(a, "--role=");
      if (std::strcmp(v, "server") == 0) c.role = Role::Server;
      else if (std::strcmp(v, "client") == 0) c.role = Role::Client;
      else return std::nullopt;
    }
    else if (starts_with(a, "--host=")) c.host = value(a, "--host=");
    else if (starts_with(a, "--port=")) c.port = static_cast<uint16_t>(std::atoi(value(a, "--port=")));
    else if (starts_with(a, "--reliable=")) {
      const char* v = value(a, "--reliable=");
      if (std::strcmp(v, "r") == 0) c.reliable = Reliability::Reliable;
      else if (std::strcmp(v, "u") == 0) c.reliable = Reliability::Unreliable;
      else if (std::strcmp(v, "na") == 0) c.reliable = Reliability::NotApplicable;
      else return std::nullopt;
    }
    else if (starts_with(a, "--size=")) c.size_bytes = std::atoi(value(a, "--size="));
    else if (starts_with(a, "--conns=")) c.conns = std::atoi(value(a, "--conns="));
    else if (starts_with(a, "--rate=")) c.rate_per_conn = std::atoi(value(a, "--rate="));
    else if (starts_with(a, "--duration=")) c.duration_s = std::atoi(value(a, "--duration="));
    else if (starts_with(a, "--warmup=")) c.warmup_s = std::atoi(value(a, "--warmup="));
    else if (starts_with(a, "--loss=")) c.loss_pct = std::atof(value(a, "--loss="));
    else if (starts_with(a, "--out=")) c.out_path = value(a, "--out=");
    else {
      std::cerr << "unknown flag: " << a << "\n";
      return std::nullopt;
    }
  }
  if (c.library.empty()) return std::nullopt;
  return c;
}

}  // namespace rudp_bench
```

- [ ] **Step 6: Update `harness/CMakeLists.txt`**

```cmake
add_library(rudp_bench_harness STATIC
  adapter_registry.cc
  scenario.cc
)
target_include_directories(rudp_bench_harness PUBLIC ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 7: Run, verify PASS**

Run: `cmake --build build -j && ctest --test-dir build -R test_scenario --output-on-failure`
Expected: 3 tests PASS

- [ ] **Step 8: Commit**

```bash
git add harness/scenario.h harness/scenario.cc harness/CMakeLists.txt \
  tests/test_scenario.cc tests/CMakeLists.txt
git commit -m "feat(harness): add scenario config CLI parser"
```

---

## Task 5: Latency / throughput / delivery metrics

**Files:**
- Create: `harness/metrics.h`
- Create: `harness/metrics.cc`
- Create: `tests/test_metrics.cc`
- Modify: `harness/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write `tests/test_metrics.cc` (failing tests)**

```cpp
#include <gtest/gtest.h>
#include "harness/metrics.h"

using namespace rudp_bench;

TEST(LatencyHist, Percentiles) {
  LatencyHist h;
  for (int i = 1; i <= 100; ++i) h.record_us(i);
  EXPECT_EQ(h.percentile_us(0.50), 50);
  EXPECT_EQ(h.percentile_us(0.95), 95);
  EXPECT_EQ(h.percentile_us(0.99), 99);
}

TEST(LatencyHist, EmptyReturnsZero) {
  LatencyHist h;
  EXPECT_EQ(h.percentile_us(0.50), 0);
}

TEST(DeliveryTracker, CountsSentAndReceived) {
  DeliveryTracker d;
  d.mark_sent(1, 0);
  d.mark_sent(2, 0);
  d.mark_sent(3, 0);
  d.mark_received(1, 0);
  d.mark_received(3, 0);
  EXPECT_EQ(d.sent(), 3u);
  EXPECT_EQ(d.received(), 2u);
  EXPECT_DOUBLE_EQ(d.delivery_ratio(), 2.0 / 3.0);
}

TEST(ThroughputCounter, BytesAndMessages) {
  ThroughputCounter t;
  t.record(64);
  t.record(64);
  t.record(64);
  EXPECT_EQ(t.bytes(), 192u);
  EXPECT_EQ(t.messages(), 3u);
}
```

- [ ] **Step 2: Update `tests/CMakeLists.txt`**

```cmake
add_executable(test_metrics test_metrics.cc)
target_link_libraries(test_metrics PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_metrics COMMAND test_metrics)
```

- [ ] **Step 3: Run, verify FAIL**

Run: `cmake --build build -j 2>&1 | tail -10`
Expected: コンパイル失敗

- [ ] **Step 4: Write `harness/metrics.h`**

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace rudp_bench {

class LatencyHist {
 public:
  void record_us(uint64_t us);
  uint64_t percentile_us(double p);  // p in [0, 1]
  size_t samples() const { return samples_.size(); }
 private:
  std::vector<uint64_t> samples_;
  bool sorted_ = false;
};

class ThroughputCounter {
 public:
  void record(size_t bytes) { bytes_ += bytes; ++messages_; }
  uint64_t bytes() const { return bytes_; }
  uint64_t messages() const { return messages_; }
 private:
  uint64_t bytes_ = 0;
  uint64_t messages_ = 0;
};

class DeliveryTracker {
 public:
  void mark_sent(uint64_t seq, uint32_t conn_id);
  void mark_received(uint64_t seq, uint32_t conn_id);
  uint64_t sent() const { return sent_count_; }
  uint64_t received() const { return received_count_; }
  double delivery_ratio() const {
    return sent_count_ ? double(received_count_) / double(sent_count_) : 0.0;
  }
 private:
  uint64_t sent_count_ = 0;
  uint64_t received_count_ = 0;
  // 重複受信は数えない
  std::unordered_set<uint64_t> received_keys_;
};

}  // namespace rudp_bench
```

- [ ] **Step 5: Write `harness/metrics.cc`**

```cpp
#include "harness/metrics.h"

#include <algorithm>

namespace rudp_bench {

void LatencyHist::record_us(uint64_t us) {
  samples_.push_back(us);
  sorted_ = false;
}

uint64_t LatencyHist::percentile_us(double p) {
  if (samples_.empty()) return 0;
  if (!sorted_) {
    std::sort(samples_.begin(), samples_.end());
    sorted_ = true;
  }
  size_t idx = static_cast<size_t>(p * (samples_.size() - 1));
  return samples_[idx];
}

static uint64_t pack(uint64_t seq, uint32_t conn_id) {
  return (static_cast<uint64_t>(conn_id) << 48) | (seq & 0x0000FFFFFFFFFFFFULL);
}

void DeliveryTracker::mark_sent(uint64_t seq, uint32_t conn_id) {
  ++sent_count_;
  (void)pack(seq, conn_id);  // 送信側は count のみ管理
}

void DeliveryTracker::mark_received(uint64_t seq, uint32_t conn_id) {
  uint64_t k = pack(seq, conn_id);
  if (received_keys_.insert(k).second) ++received_count_;
}

}  // namespace rudp_bench
```

- [ ] **Step 6: Update `harness/CMakeLists.txt`**

```cmake
add_library(rudp_bench_harness STATIC
  adapter_registry.cc
  scenario.cc
  metrics.cc
)
target_include_directories(rudp_bench_harness PUBLIC ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 7: Run, verify PASS**

Run: `cmake --build build -j && ctest --test-dir build -R test_metrics --output-on-failure`
Expected: 4 tests PASS

- [ ] **Step 8: Commit**

```bash
git add harness/metrics.h harness/metrics.cc harness/CMakeLists.txt \
  tests/test_metrics.cc tests/CMakeLists.txt
git commit -m "feat(harness): add latency hist / throughput / delivery metrics"
```

---

## Task 6: CSV writer

**Files:**
- Create: `harness/csv_writer.h`
- Create: `harness/csv_writer.cc`
- Create: `tests/test_csv_writer.cc`
- Modify: `harness/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write `tests/test_csv_writer.cc` (failing test)**

```cpp
#include <gtest/gtest.h>
#include "harness/csv_writer.h"

#include <fstream>
#include <sstream>

using namespace rudp_bench;

TEST(CsvWriter, WritesHeaderAndRow) {
  CsvRow r;
  r.library = "raw_udp";
  r.encryption = "off";
  r.phase = 1;
  r.reliable = "u";
  r.size = 64;
  r.conns = 1;
  r.rate = 100;
  r.loss = 0.0;
  r.throughput_mbps = 12.5;
  r.msg_per_sec = 24414;
  r.rtt_p50_us = 25;
  r.rtt_p95_us = 80;
  r.rtt_p99_us = 200;
  r.delivered = 1000;
  r.sent = 1000;
  r.delivery_ratio = 1.0;
  r.cpu_pct = 5.5;
  r.rss_mb = 12;
  r.connect_ms = 0;
  r.duration_s = 30;

  std::ostringstream os;
  write_header(os);
  write_row(os, r);

  std::string out = os.str();
  EXPECT_NE(out.find("library,encryption,phase,reliable"), std::string::npos);
  EXPECT_NE(out.find("raw_udp,off,1,u,64,1,100,0.000"), std::string::npos);
}
```

- [ ] **Step 2: Update `tests/CMakeLists.txt`**

```cmake
add_executable(test_csv_writer test_csv_writer.cc)
target_link_libraries(test_csv_writer PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_csv_writer COMMAND test_csv_writer)
```

- [ ] **Step 3: Run, verify FAIL**

Run: `cmake --build build -j 2>&1 | tail -10`
Expected: コンパイル失敗

- [ ] **Step 4: Write `harness/csv_writer.h`**

```cpp
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace rudp_bench {

struct CsvRow {
  std::string library;
  std::string encryption;       // "on"/"off"
  int phase = 1;
  std::string reliable;          // "r"/"u"/"na"
  uint32_t size = 0;
  uint32_t conns = 0;
  uint32_t rate = 0;
  double loss = 0.0;
  double throughput_mbps = 0.0;
  uint64_t msg_per_sec = 0;
  uint64_t rtt_p50_us = 0;
  uint64_t rtt_p95_us = 0;
  uint64_t rtt_p99_us = 0;
  uint64_t delivered = 0;
  uint64_t sent = 0;
  double delivery_ratio = 0.0;
  double cpu_pct = 0.0;
  uint64_t rss_mb = 0;
  uint64_t connect_ms = 0;
  uint32_t duration_s = 0;
};

void write_header(std::ostream& os);
void write_row(std::ostream& os, const CsvRow& r);

}  // namespace rudp_bench
```

- [ ] **Step 5: Write `harness/csv_writer.cc`**

```cpp
#include "harness/csv_writer.h"

#include <iomanip>
#include <ostream>

namespace rudp_bench {

void write_header(std::ostream& os) {
  os << "library,encryption,phase,reliable,size,conns,rate,loss,"
     << "throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,"
     << "delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s\n";
}

void write_row(std::ostream& os, const CsvRow& r) {
  os << r.library << ',' << r.encryption << ',' << r.phase << ',' << r.reliable << ','
     << r.size << ',' << r.conns << ',' << r.rate << ','
     << std::fixed << std::setprecision(3) << r.loss << ','
     << std::setprecision(3) << r.throughput_mbps << ','
     << r.msg_per_sec << ','
     << r.rtt_p50_us << ',' << r.rtt_p95_us << ',' << r.rtt_p99_us << ','
     << r.delivered << ',' << r.sent << ','
     << std::setprecision(4) << r.delivery_ratio << ','
     << std::setprecision(2) << r.cpu_pct << ','
     << r.rss_mb << ',' << r.connect_ms << ',' << r.duration_s << '\n';
}

}  // namespace rudp_bench
```

- [ ] **Step 6: Update `harness/CMakeLists.txt`**

```cmake
add_library(rudp_bench_harness STATIC
  adapter_registry.cc
  scenario.cc
  metrics.cc
  csv_writer.cc
)
target_include_directories(rudp_bench_harness PUBLIC ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 7: Run, verify PASS**

Run: `cmake --build build -j && ctest --test-dir build -R test_csv_writer --output-on-failure`
Expected: 1 test PASS

- [ ] **Step 8: Commit**

```bash
git add harness/csv_writer.h harness/csv_writer.cc harness/CMakeLists.txt \
  tests/test_csv_writer.cc tests/CMakeLists.txt
git commit -m "feat(harness): add CSV row writer"
```

---

## Task 7: Process resource sampler (CPU + RSS)

**Files:**
- Create: `harness/proc_sampler.h`
- Create: `harness/proc_sampler.cc`
- Create: `tests/test_proc_sampler.cc`
- Modify: `harness/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write `tests/test_proc_sampler.cc` (failing test)**

```cpp
#include <gtest/gtest.h>
#include "harness/proc_sampler.h"

#include <chrono>
#include <thread>

using namespace rudp_bench;

TEST(ProcSampler, CpuTimeIncreasesUnderBusyWork) {
  ProcSampler s;
  s.begin();
  // ~100ms ぶん CPU を回す
  auto t0 = std::chrono::steady_clock::now();
  volatile uint64_t x = 0;
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100)) {
    for (int i = 0; i < 100000; ++i) x += i;
  }
  s.end();
  EXPECT_GT(s.cpu_pct(), 0.0);
}

TEST(ProcSampler, RssReadable) {
  ProcSampler s;
  s.begin();
  s.end();
  EXPECT_GT(s.rss_max_mb(), 0u);
}
```

- [ ] **Step 2: Update `tests/CMakeLists.txt`**

```cmake
add_executable(test_proc_sampler test_proc_sampler.cc)
target_link_libraries(test_proc_sampler PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_proc_sampler COMMAND test_proc_sampler)
```

- [ ] **Step 3: Run, verify FAIL**

Run: `cmake --build build -j 2>&1 | tail -10`
Expected: コンパイル失敗

- [ ] **Step 4: Write `harness/proc_sampler.h`**

```cpp
#pragma once

#include <chrono>
#include <cstdint>

namespace rudp_bench {

class ProcSampler {
 public:
  void begin();
  void end();
  double cpu_pct() const;        // (cpu_time / wall_time) * 100
  uint64_t rss_max_mb() const;
 private:
  std::chrono::steady_clock::time_point t0_{};
  std::chrono::steady_clock::time_point t1_{};
  uint64_t cpu_us_begin_ = 0;
  uint64_t cpu_us_end_ = 0;
  uint64_t rss_mb_max_ = 0;
};

}  // namespace rudp_bench
```

- [ ] **Step 5: Write `harness/proc_sampler.cc`**

```cpp
#include "harness/proc_sampler.h"

#include <fstream>
#include <string>
#include <sys/resource.h>

namespace rudp_bench {
namespace {

uint64_t cpu_us_now() {
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
  uint64_t u = static_cast<uint64_t>(r.ru_utime.tv_sec) * 1'000'000ULL + r.ru_utime.tv_usec;
  uint64_t s = static_cast<uint64_t>(r.ru_stime.tv_sec) * 1'000'000ULL + r.ru_stime.tv_usec;
  return u + s;
}

uint64_t rss_kb_now() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      uint64_t kb = 0;
      for (char c : line) if (c >= '0' && c <= '9') { kb = kb * 10 + (c - '0'); }
      return kb;
    }
  }
  return 0;
}

}  // namespace

void ProcSampler::begin() {
  t0_ = std::chrono::steady_clock::now();
  cpu_us_begin_ = cpu_us_now();
  rss_mb_max_ = rss_kb_now() / 1024;
}

void ProcSampler::end() {
  t1_ = std::chrono::steady_clock::now();
  cpu_us_end_ = cpu_us_now();
  uint64_t now = rss_kb_now() / 1024;
  if (now > rss_mb_max_) rss_mb_max_ = now;
}

double ProcSampler::cpu_pct() const {
  uint64_t cpu_us = cpu_us_end_ - cpu_us_begin_;
  uint64_t wall_us = std::chrono::duration_cast<std::chrono::microseconds>(t1_ - t0_).count();
  if (wall_us == 0) return 0.0;
  return 100.0 * static_cast<double>(cpu_us) / static_cast<double>(wall_us);
}

uint64_t ProcSampler::rss_max_mb() const { return rss_mb_max_; }

}  // namespace rudp_bench
```

- [ ] **Step 6: Update `harness/CMakeLists.txt`**

```cmake
add_library(rudp_bench_harness STATIC
  adapter_registry.cc
  scenario.cc
  metrics.cc
  csv_writer.cc
  proc_sampler.cc
)
target_include_directories(rudp_bench_harness PUBLIC ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 7: Run, verify PASS**

Run: `cmake --build build -j && ctest --test-dir build -R test_proc_sampler --output-on-failure`
Expected: 2 tests PASS

- [ ] **Step 8: Commit**

```bash
git add harness/proc_sampler.h harness/proc_sampler.cc harness/CMakeLists.txt \
  tests/test_proc_sampler.cc tests/CMakeLists.txt
git commit -m "feat(harness): add CPU/RSS sampler"
```

---

## Task 8: raw_udp adapter

**Files:**
- Create: `adapters/raw_udp/raw_udp_adapter.cc`
- Create: `tests/test_raw_udp_smoke.cc`
- Modify: `adapters/raw_udp/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

各メッセージは UDP datagram としてそのまま送る。クライアントは N 個の `socket()` を作り、`connect()` で固定ピアにつなぐ。サーバは 1 つの listening socket で `recvfrom()` してエコーする(送信元アドレスをキーに `conn_id` を割り当て、`std::unordered_map<sockaddr, uint32_t>`)。

- [ ] **Step 1: Write `tests/test_raw_udp_smoke.cc` (failing test)**

```cpp
#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <thread>
#include <chrono>
#include <cstring>

using namespace rudp_bench;

TEST(RawUdpSmoke, EchoOneMessage) {
  auto server = create_adapter("raw_udp");
  auto client = create_adapter("raw_udp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(0xC100);

  std::thread server_thread([&]() {
    char buf[1024];
    size_t len;
    uint32_t conn_id;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      int r = server->recv(buf, sizeof(buf), &len, &conn_id);
      if (r == 1) {
        server->send(conn_id, buf, len, false);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t conn_id = client->client_connect("127.0.0.1", 0xC100);
  ASSERT_TRUE(client->is_connected(conn_id));

  const char msg[] = "hello";
  ASSERT_EQ(client->send(conn_id, msg, sizeof(msg), false), 0);

  char buf[1024]; size_t len; uint32_t in_conn;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    int r = client->recv(buf, sizeof(buf), &len, &in_conn);
    if (r == 1) {
      EXPECT_EQ(in_conn, conn_id);
      EXPECT_EQ(len, sizeof(msg));
      EXPECT_STREQ(buf, msg);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(got);

  server_thread.join();
  client->close();
  server->close();
}

TEST(RawUdpSmoke, ReportsCapability) {
  auto a = create_adapter("raw_udp");
  EXPECT_TRUE(a->supports(false));
  EXPECT_FALSE(a->supports(true));
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "raw_udp");
}
```

- [ ] **Step 2: Update `adapters/raw_udp/CMakeLists.txt`**

```cmake
add_library(adapter_raw_udp STATIC raw_udp_adapter.cc)
target_link_libraries(adapter_raw_udp PUBLIC rudp_bench_harness)
target_compile_options(adapter_raw_udp PRIVATE -Wall -Wextra)
```

- [ ] **Step 3: Update `tests/CMakeLists.txt`**

```cmake
add_executable(test_raw_udp_smoke test_raw_udp_smoke.cc)
target_link_libraries(test_raw_udp_smoke PRIVATE adapter_raw_udp GTest::gtest_main)
add_test(NAME test_raw_udp_smoke COMMAND test_raw_udp_smoke)
```

- [ ] **Step 4: Run, verify FAIL**

Run: `cmake --build build -j 2>&1 | tail -10`
Expected: コンパイル失敗(adapter_raw_udp ライブラリにシンボル無し)

- [ ] **Step 5: Write `adapters/raw_udp/raw_udp_adapter.cc`**

```cpp
#include "harness/adapter.h"
#include "harness/adapter_registry.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

struct Conn {
  int fd = -1;
  sockaddr_in peer{};
};

uint64_t addr_key(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 32) | static_cast<uint64_t>(a.sin_port);
}

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class RawUdpAdapter : public rudp_bench::Adapter {
 public:
  void server_listen(uint16_t port) override {
    server_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    ::bind(server_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    set_nonblock(server_fd_);
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    Conn c;
    c.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    set_nonblock(c.fd);
    c.peer.sin_family = AF_INET;
    c.peer.sin_port = htons(port);
    inet_pton(AF_INET, host, &c.peer.sin_addr);
    uint32_t id = next_id_++;
    conns_[id] = c;
    return id;
  }

  bool is_connected(uint32_t) override { return true; }

  int send(uint32_t conn_id, const void* data, size_t len, bool /*reliable*/) override {
    if (server_fd_ >= 0) {
      auto it = peer_by_id_.find(conn_id);
      if (it == peer_by_id_.end()) return -1;
      ssize_t n = ::sendto(server_fd_, data, len, 0,
                          reinterpret_cast<sockaddr*>(&it->second), sizeof(it->second));
      return (n == static_cast<ssize_t>(len)) ? 0 : -1;
    }
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return -1;
    ssize_t n = ::sendto(it->second.fd, data, len, 0,
                        reinterpret_cast<sockaddr*>(&it->second.peer),
                        sizeof(it->second.peer));
    return (n == static_cast<ssize_t>(len)) ? 0 : -1;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    if (server_fd_ >= 0) {
      sockaddr_in src{};
      socklen_t sl = sizeof(src);
      ssize_t n = ::recvfrom(server_fd_, buf, cap, 0,
                            reinterpret_cast<sockaddr*>(&src), &sl);
      if (n <= 0) return 0;
      uint64_t k = addr_key(src);
      auto it = id_by_key_.find(k);
      uint32_t id;
      if (it == id_by_key_.end()) {
        id = next_id_++;
        id_by_key_[k] = id;
        peer_by_id_[id] = src;
      } else {
        id = it->second;
      }
      *out_len = static_cast<size_t>(n);
      *out_conn_id = id;
      return 1;
    }
    for (auto& [id, c] : conns_) {
      ssize_t n = ::recv(c.fd, buf, cap, 0);
      if (n > 0) {
        *out_len = static_cast<size_t>(n);
        *out_conn_id = id;
        return 1;
      }
    }
    return 0;
  }

  void poll() override {}

  void close() override {
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    for (auto& [id, c] : conns_) ::close(c.fd);
    conns_.clear();
  }

  const char* name() const override { return "raw_udp"; }
  bool supports(bool reliable) const override { return !reliable; }
  bool encryption_on() const override { return false; }

 private:
  int server_fd_ = -1;
  std::unordered_map<uint32_t, Conn> conns_;
  std::unordered_map<uint64_t, uint32_t> id_by_key_;
  std::unordered_map<uint32_t, sockaddr_in> peer_by_id_;
  uint32_t next_id_ = 1;
};

}  // namespace

namespace rudp_bench {
void register_raw_udp_adapter() {
  register_adapter("raw_udp",
      []() { return std::make_unique<RawUdpAdapter>(); });
}
}  // namespace rudp_bench
```

- [ ] **Step 6: Add registration call to smoke test**

`tests/test_raw_udp_smoke.cc` の最初に追加(各 TEST_F より前):

```cpp
namespace rudp_bench { void register_raw_udp_adapter(); }

class RawUdpRegistrar {
 public:
  RawUdpRegistrar() { rudp_bench::register_raw_udp_adapter(); }
};
static RawUdpRegistrar registrar;
```

- [ ] **Step 7: Run, verify PASS**

Run: `cmake --build build -j && ctest --test-dir build -R test_raw_udp_smoke --output-on-failure`
Expected: 2 tests PASS

- [ ] **Step 8: Commit**

```bash
git add adapters/raw_udp/raw_udp_adapter.cc adapters/raw_udp/CMakeLists.txt \
  tests/test_raw_udp_smoke.cc tests/CMakeLists.txt
git commit -m "feat(adapter): add raw_udp baseline adapter"
```

---

## Task 9: mini_rudp adapter

**Files:**
- Create: `adapters/mini_rudp/mini_rudp_adapter.cc`
- Create: `tests/test_mini_rudp_smoke.cc`
- Modify: `adapters/mini_rudp/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

100行で書ける素朴 RUDP。各 datagram に `[2B flags][4B seq][N payload]` のヘッダを付ける。
- flags bit0 = ACK, bit1 = RELIABLE
- 送信側: reliable はリトライキューに追加。`poll()` で 50ms 経過した未ACKを再送
- 受信側: reliable を受けたら ACK datagram を返す。受信済み seq セットで重複排除

- [ ] **Step 1: Write `tests/test_mini_rudp_smoke.cc` (failing test)**

```cpp
#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <thread>

using namespace rudp_bench;

TEST(MiniRudpSmoke, ReliableEcho) {
  auto server = create_adapter("mini_rudp");
  auto client = create_adapter("mini_rudp");
  ASSERT_NE(server, nullptr);
  server->server_listen(0xC101);

  std::thread server_thread([&]() {
    char buf[2048]; size_t len; uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC101);
  const char msg[] = "rudp-hello";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  char buf[2048]; size_t len; uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(len, sizeof(msg));
      EXPECT_STREQ(buf, msg);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(got);
  server_thread.join();
  client->close();
  server->close();
}

TEST(MiniRudpSmoke, Capability) {
  auto a = create_adapter("mini_rudp");
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "mini_rudp");
}
```

- [ ] **Step 2: Update `adapters/mini_rudp/CMakeLists.txt`**

```cmake
add_library(adapter_mini_rudp STATIC mini_rudp_adapter.cc)
target_link_libraries(adapter_mini_rudp PUBLIC rudp_bench_harness)
```

- [ ] **Step 3: Update `tests/CMakeLists.txt`**

```cmake
add_executable(test_mini_rudp_smoke test_mini_rudp_smoke.cc)
target_link_libraries(test_mini_rudp_smoke PRIVATE adapter_mini_rudp GTest::gtest_main)
add_test(NAME test_mini_rudp_smoke COMMAND test_mini_rudp_smoke)
```

- [ ] **Step 4: Run, verify FAIL**

Run: `cmake --build build -j 2>&1 | tail -10`
Expected: コンパイル失敗

- [ ] **Step 5: Write `adapters/mini_rudp/mini_rudp_adapter.cc`**

```cpp
#include "harness/adapter.h"
#include "harness/adapter_registry.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint16_t FLAG_ACK = 1;
constexpr uint16_t FLAG_REL = 2;

struct Header {
  uint16_t flags;
  uint32_t seq;
} __attribute__((packed));

struct PendingSend {
  std::vector<uint8_t> bytes;
  std::chrono::steady_clock::time_point sent_at;
};

struct Conn {
  int fd = -1;
  sockaddr_in peer{};
  uint32_t next_seq = 1;
  std::unordered_map<uint32_t, PendingSend> pending;     // 未 ACK
  std::unordered_set<uint32_t> received_seq;             // 重複排除
};

uint64_t addr_key(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 32) |
         static_cast<uint64_t>(a.sin_port);
}

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class MiniRudpAdapter : public rudp_bench::Adapter {
 public:
  void server_listen(uint16_t port) override {
    server_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    ::bind(server_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    set_nonblock(server_fd_);
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    Conn c;
    c.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    set_nonblock(c.fd);
    c.peer.sin_family = AF_INET;
    c.peer.sin_port = htons(port);
    inet_pton(AF_INET, host, &c.peer.sin_addr);
    uint32_t id = next_id_++;
    conns_[id] = std::move(c);
    return id;
  }

  bool is_connected(uint32_t) override { return true; }

  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    auto* c = find_conn(conn_id);
    if (!c) return -1;
    Header h;
    h.flags = reliable ? FLAG_REL : 0;
    h.seq = c->next_seq++;
    std::vector<uint8_t> pkt(sizeof(h) + len);
    std::memcpy(pkt.data(), &h, sizeof(h));
    std::memcpy(pkt.data() + sizeof(h), data, len);
    int sent_fd = (server_fd_ >= 0) ? server_fd_ : c->fd;
    sockaddr_in& peer = c->peer;
    ssize_t n = ::sendto(sent_fd, pkt.data(), pkt.size(), 0,
                        reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    if (n != static_cast<ssize_t>(pkt.size())) return -1;
    if (reliable) {
      c->pending[h.seq] = {std::move(pkt), std::chrono::steady_clock::now()};
    }
    return 0;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    sockaddr_in src{};
    socklen_t sl = sizeof(src);
    int fd = (server_fd_ >= 0) ? server_fd_ : -1;
    if (fd < 0) {
      // client: poll all conn fds
      for (auto& [id, c] : conns_) {
        sockaddr_in s{}; socklen_t s_sl = sizeof(s);
        uint8_t pkt[2048];
        ssize_t n = ::recvfrom(c.fd, pkt, sizeof(pkt), 0,
                              reinterpret_cast<sockaddr*>(&s), &s_sl);
        if (n <= 0) continue;
        if (handle_packet(c, pkt, n, buf, cap, out_len)) {
          *out_conn_id = id;
          return 1;
        }
      }
      return 0;
    }
    // server: single fd
    uint8_t pkt[2048];
    ssize_t n = ::recvfrom(fd, pkt, sizeof(pkt), 0,
                          reinterpret_cast<sockaddr*>(&src), &sl);
    if (n <= 0) return 0;
    uint64_t k = addr_key(src);
    auto it = id_by_key_.find(k);
    uint32_t id;
    if (it == id_by_key_.end()) {
      id = next_id_++;
      id_by_key_[k] = id;
      Conn c;
      c.peer = src;
      conns_[id] = std::move(c);
    } else {
      id = it->second;
    }
    if (handle_packet(conns_[id], pkt, n, buf, cap, out_len)) {
      *out_conn_id = id;
      return 1;
    }
    return 0;
  }

  void poll() override {
    auto now = std::chrono::steady_clock::now();
    for (auto& [id, c] : conns_) {
      for (auto& [seq, ps] : c.pending) {
        if (now - ps.sent_at > std::chrono::milliseconds(50)) {
          int fd = (server_fd_ >= 0) ? server_fd_ : c.fd;
          ::sendto(fd, ps.bytes.data(), ps.bytes.size(), 0,
                  reinterpret_cast<sockaddr*>(&c.peer), sizeof(c.peer));
          ps.sent_at = now;
        }
      }
    }
  }

  void close() override {
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    for (auto& [id, c] : conns_) if (c.fd >= 0) ::close(c.fd);
    conns_.clear();
  }

  const char* name() const override { return "mini_rudp"; }
  bool supports(bool) const override { return true; }
  bool encryption_on() const override { return false; }

 private:
  Conn* find_conn(uint32_t id) {
    auto it = conns_.find(id);
    return it == conns_.end() ? nullptr : &it->second;
  }

  bool handle_packet(Conn& c, const uint8_t* pkt, ssize_t n,
                     void* buf, size_t cap, size_t* out_len) {
    if (n < static_cast<ssize_t>(sizeof(Header))) return false;
    Header h;
    std::memcpy(&h, pkt, sizeof(h));
    size_t payload = static_cast<size_t>(n) - sizeof(h);
    if (h.flags & FLAG_ACK) {
      c.pending.erase(h.seq);
      return false;
    }
    if (h.flags & FLAG_REL) {
      Header ack{};
      ack.flags = FLAG_ACK;
      ack.seq = h.seq;
      int fd = (server_fd_ >= 0) ? server_fd_ : c.fd;
      ::sendto(fd, &ack, sizeof(ack), 0,
              reinterpret_cast<sockaddr*>(&c.peer), sizeof(c.peer));
      if (!c.received_seq.insert(h.seq).second) return false;
    }
    if (payload > cap) return false;
    std::memcpy(buf, pkt + sizeof(h), payload);
    *out_len = payload;
    return true;
  }

  int server_fd_ = -1;
  std::unordered_map<uint32_t, Conn> conns_;
  std::unordered_map<uint64_t, uint32_t> id_by_key_;
  uint32_t next_id_ = 1;
};

}  // namespace

namespace rudp_bench {
void register_mini_rudp_adapter() {
  register_adapter("mini_rudp",
      []() { return std::make_unique<MiniRudpAdapter>(); });
}
}  // namespace rudp_bench
```

- [ ] **Step 6: Add registration call to smoke test**

`tests/test_mini_rudp_smoke.cc` の冒頭に追加:

```cpp
namespace rudp_bench { void register_mini_rudp_adapter(); }

class MiniRudpRegistrar {
 public:
  MiniRudpRegistrar() { rudp_bench::register_mini_rudp_adapter(); }
};
static MiniRudpRegistrar registrar;
```

同様に `tests/test_runner_loopback.cc` (Task 11) でも `register_raw_udp_adapter()` を呼ぶ必要がある。Task 11 の test ファイルにも同じパターンを追加すること。

- [ ] **Step 7: Run, verify PASS**

Run: `cmake --build build -j && ctest --test-dir build -R test_mini_rudp_smoke --output-on-failure`
Expected: 2 tests PASS

- [ ] **Step 8: Commit**

```bash
git add adapters/mini_rudp/mini_rudp_adapter.cc adapters/mini_rudp/CMakeLists.txt \
  tests/test_mini_rudp_smoke.cc tests/CMakeLists.txt
git commit -m "feat(adapter): add mini_rudp baseline adapter"
```

---

## Task 10: Runner — server loop

**Files:**
- Create: `harness/runner.h`
- Create: `harness/runner.cc`
- Modify: `harness/CMakeLists.txt`

サーバはひたすらエコーするだけ。本ランからの全体時間と CPU/RSS を採取して CSV を吐く。

- [ ] **Step 1: Write `harness/runner.h`**

```cpp
#pragma once

#include "harness/adapter.h"
#include "harness/csv_writer.h"
#include "harness/scenario.h"

namespace rudp_bench {

CsvRow run_server(Adapter& a, const ScenarioConfig& cfg);
CsvRow run_client(Adapter& a, const ScenarioConfig& cfg);

}  // namespace rudp_bench
```

- [ ] **Step 2: Write `harness/runner.cc` (server only first)**

```cpp
#include "harness/runner.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "harness/metrics.h"
#include "harness/proc_sampler.h"

namespace rudp_bench {

CsvRow run_server(Adapter& a, const ScenarioConfig& cfg) {
  ProcSampler ps;
  a.server_listen(cfg.port);
  ps.begin();

  std::vector<uint8_t> buf(65536);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(cfg.duration_s + cfg.warmup_s + 5);
  while (std::chrono::steady_clock::now() < deadline) {
    a.poll();
    size_t n; uint32_t cid;
    int r = a.recv(buf.data(), buf.size(), &n, &cid);
    if (r == 1) {
      bool reliable = (cfg.reliable == Reliability::Reliable);
      a.send(cid, buf.data(), n, reliable);
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  ps.end();
  a.close();

  CsvRow row;
  row.library = cfg.library;
  row.encryption = a.encryption_on() ? "on" : "off";
  row.phase = 1;
  row.reliable = cfg.reliable == Reliability::Reliable ? "r" :
                 cfg.reliable == Reliability::Unreliable ? "u" : "na";
  row.size = cfg.size_bytes;
  row.conns = cfg.conns;
  row.rate = cfg.rate_per_conn;
  row.loss = cfg.loss_pct;
  row.cpu_pct = ps.cpu_pct();
  row.rss_mb = ps.rss_max_mb();
  row.duration_s = cfg.duration_s;
  return row;
}

// client は次のタスクで実装
CsvRow run_client(Adapter&, const ScenarioConfig&) { return {}; }

}  // namespace rudp_bench
```

- [ ] **Step 3: Update `harness/CMakeLists.txt`**

```cmake
add_library(rudp_bench_harness STATIC
  adapter_registry.cc
  scenario.cc
  metrics.cc
  csv_writer.cc
  proc_sampler.cc
  runner.cc
)
target_include_directories(rudp_bench_harness PUBLIC ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 4: Verify compile**

Run: `cmake --build build -j`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add harness/runner.h harness/runner.cc harness/CMakeLists.txt
git commit -m "feat(runner): add server echo loop"
```

---

## Task 11: Runner — client loop with rate pacing

**Files:**
- Modify: `harness/runner.cc` (replace stub `run_client`)
- Create: `tests/test_runner_loopback.cc`
- Modify: `tests/CMakeLists.txt`

メッセージは `[8B seq][8B send_ts_ns][padding...]`。クライアントは N conns を作り、各 conn でレート (msg/sec) に従い送信。受信時に send_ts と現在時刻から RTT 算出し `LatencyHist` に記録。warmup 中は計測除外。

- [ ] **Step 1: Write `tests/test_runner_loopback.cc` (failing test)**

```cpp
#include <gtest/gtest.h>

#include "harness/adapter_registry.h"
#include "harness/runner.h"

#include <thread>

namespace rudp_bench { void register_raw_udp_adapter(); }

class RawUdpRegistrar {
 public:
  RawUdpRegistrar() { rudp_bench::register_raw_udp_adapter(); }
};
static RawUdpRegistrar registrar;

using namespace rudp_bench;

TEST(RunnerLoopback, RawUdpShortSession) {
  ScenarioConfig sc;
  sc.library = "raw_udp";
  sc.role = Role::Server;
  sc.port = 0xC110;
  sc.reliable = Reliability::Unreliable;
  sc.duration_s = 2;
  sc.warmup_s = 0;

  ScenarioConfig cc = sc;
  cc.role = Role::Client;
  cc.host = "127.0.0.1";
  cc.size_bytes = 64;
  cc.conns = 1;
  cc.rate_per_conn = 100;

  auto server = create_adapter("raw_udp");
  auto client = create_adapter("raw_udp");

  std::thread st([&]() { run_server(*server, sc); });
  // give server a beat
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CsvRow row = run_client(*client, cc);
  st.join();

  EXPECT_GT(row.sent, 100u);
  EXPECT_GT(row.delivered, 50u);
  EXPECT_GT(row.delivery_ratio, 0.5);
}
```

- [ ] **Step 2: Update `tests/CMakeLists.txt`**

```cmake
add_executable(test_runner_loopback test_runner_loopback.cc)
target_link_libraries(test_runner_loopback PRIVATE adapter_raw_udp GTest::gtest_main)
add_test(NAME test_runner_loopback COMMAND test_runner_loopback)
```

- [ ] **Step 3: Run, verify FAIL**

Run: `cmake --build build -j 2>&1 | tail -10`
Expected: 通信成功するが期待値で失敗(`run_client` が空行返す)

- [ ] **Step 4: Implement client loop in `harness/runner.cc`**

`run_client` を以下で置き換え:

```cpp
CsvRow run_client(Adapter& a, const ScenarioConfig& cfg) {
  using clock = std::chrono::steady_clock;
  ProcSampler ps;
  LatencyHist rtt;
  ThroughputCounter th;
  DeliveryTracker dt;

  // connect all
  std::vector<uint32_t> ids;
  ids.reserve(cfg.conns);
  auto t_connect_begin = clock::now();
  for (uint32_t i = 0; i < cfg.conns; ++i) {
    ids.push_back(a.client_connect(cfg.host.c_str(), cfg.port));
  }
  // 全コネクションが ready になるまで poll
  for (auto id : ids) {
    while (!a.is_connected(id)) {
      a.poll();
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
  auto t_connect_end = clock::now();
  uint64_t connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            t_connect_end - t_connect_begin).count();

  ps.begin();
  auto warmup_end = clock::now() + std::chrono::seconds(cfg.warmup_s);
  auto run_end = warmup_end + std::chrono::seconds(cfg.duration_s);

  // pacing: 各 conn で次回送信時刻を保持
  std::vector<clock::time_point> next_send(cfg.conns, clock::now());
  std::chrono::nanoseconds interval =
      cfg.rate_per_conn ? std::chrono::nanoseconds(1'000'000'000ULL / cfg.rate_per_conn)
                        : std::chrono::nanoseconds(0);

  std::vector<uint8_t> payload(cfg.size_bytes, 0xAB);
  std::vector<uint64_t> seq_counter(cfg.conns, 1);
  std::vector<uint8_t> rxbuf(65536);

  bool reliable = (cfg.reliable == Reliability::Reliable);
  auto in_measure = [&](clock::time_point t) { return t >= warmup_end; };

  // tail drain: 送信終了後しばらく recv だけ
  auto tail_until = run_end + std::chrono::milliseconds(500);

  while (clock::now() < tail_until) {
    auto now = clock::now();
    if (now < run_end) {
      for (uint32_t i = 0; i < cfg.conns; ++i) {
        if (now < next_send[i]) continue;
        // ヘッダ書き込み
        uint64_t seq = seq_counter[i]++;
        uint64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          now.time_since_epoch()).count();
        std::memcpy(payload.data(), &seq, 8);
        std::memcpy(payload.data() + 8, &ts, 8);
        if (a.send(ids[i], payload.data(), payload.size(), reliable) == 0) {
          if (in_measure(now)) {
            dt.mark_sent(seq, ids[i]);
          }
        }
        if (cfg.rate_per_conn) {
          next_send[i] += interval;
          if (next_send[i] < now) next_send[i] = now;  // catch-up cap
        }
      }
    }
    a.poll();
    while (true) {
      size_t n; uint32_t cid;
      int r = a.recv(rxbuf.data(), rxbuf.size(), &n, &cid);
      if (r != 1) break;
      if (n < 16) continue;
      uint64_t seq, ts;
      std::memcpy(&seq, rxbuf.data(), 8);
      std::memcpy(&ts, rxbuf.data() + 8, 8);
      auto t_recv = clock::now();
      uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            t_recv.time_since_epoch()).count();
      uint64_t rtt_us = (now_ns - ts) / 1000;
      if (in_measure(t_recv)) {
        rtt.record_us(rtt_us);
        th.record(n);
        dt.mark_received(seq, cid);
      }
    }
  }
  ps.end();
  a.close();

  CsvRow row;
  row.library = cfg.library;
  row.encryption = a.encryption_on() ? "on" : "off";
  row.phase = 1;
  row.reliable = reliable ? "r"
                          : (cfg.reliable == Reliability::Unreliable ? "u" : "na");
  row.size = cfg.size_bytes;
  row.conns = cfg.conns;
  row.rate = cfg.rate_per_conn;
  row.loss = cfg.loss_pct;
  row.throughput_mbps = (th.bytes() * 8.0) / (cfg.duration_s * 1'000'000.0);
  row.msg_per_sec = th.messages() / std::max<uint32_t>(1, cfg.duration_s);
  row.rtt_p50_us = rtt.percentile_us(0.50);
  row.rtt_p95_us = rtt.percentile_us(0.95);
  row.rtt_p99_us = rtt.percentile_us(0.99);
  row.delivered = dt.received();
  row.sent = dt.sent();
  row.delivery_ratio = dt.delivery_ratio();
  row.cpu_pct = ps.cpu_pct();
  row.rss_mb = ps.rss_max_mb();
  row.connect_ms = connect_ms;
  row.duration_s = cfg.duration_s;
  return row;
}
```

`runner.cc` トップに include を追加:
```cpp
#include <algorithm>
```

- [ ] **Step 5: Run, verify PASS**

Run: `cmake --build build -j && ctest --test-dir build -R test_runner_loopback --output-on-failure`
Expected: 1 test PASS

- [ ] **Step 6: Commit**

```bash
git add harness/runner.cc tests/test_runner_loopback.cc tests/CMakeLists.txt
git commit -m "feat(runner): add client loop with rate pacing and RTT measurement"
```

---

## Task 12: CLI main + binary build

**Files:**
- Create: `harness/main.cc`
- Modify: `harness/CMakeLists.txt`
- Modify: top `CMakeLists.txt` (add executable)

- [ ] **Step 1: Write `harness/main.cc`**

```cpp
#include <fstream>
#include <iostream>

#include "harness/adapter_registry.h"
#include "harness/csv_writer.h"
#include "harness/runner.h"
#include "harness/scenario.h"

namespace rudp_bench {
void register_raw_udp_adapter();
void register_mini_rudp_adapter();
}  // namespace rudp_bench

int main(int argc, const char* argv[]) {
  rudp_bench::register_raw_udp_adapter();
  rudp_bench::register_mini_rudp_adapter();

  auto cfg_opt = rudp_bench::parse_scenario(argc, argv);
  if (!cfg_opt) {
    std::cerr << "usage: rudp-bench --library=<name> --role=server|client ...\n";
    return 2;
  }
  auto& cfg = *cfg_opt;

  auto adapter = rudp_bench::create_adapter(cfg.library);
  if (!adapter) {
    std::cerr << "unknown library: " << cfg.library << "\n";
    return 2;
  }

  // capability check
  if (cfg.reliable == rudp_bench::Reliability::Reliable && !adapter->supports(true)) {
    std::cerr << "library " << cfg.library << " does not support reliable; emit na row\n";
    rudp_bench::CsvRow row;
    row.library = cfg.library;
    row.encryption = adapter->encryption_on() ? "on" : "off";
    row.reliable = "na";
    row.size = cfg.size_bytes;
    row.conns = cfg.conns;
    row.rate = cfg.rate_per_conn;
    row.loss = cfg.loss_pct;
    row.duration_s = cfg.duration_s;
    if (!cfg.out_path.empty()) {
      std::ofstream f(cfg.out_path);
      rudp_bench::write_header(f);
      rudp_bench::write_row(f, row);
    } else {
      rudp_bench::write_header(std::cout);
      rudp_bench::write_row(std::cout, row);
    }
    return 0;
  }

  rudp_bench::CsvRow row =
      cfg.role == rudp_bench::Role::Server
          ? rudp_bench::run_server(*adapter, cfg)
          : rudp_bench::run_client(*adapter, cfg);

  if (!cfg.out_path.empty()) {
    std::ofstream f(cfg.out_path);
    rudp_bench::write_header(f);
    rudp_bench::write_row(f, row);
  } else {
    rudp_bench::write_header(std::cout);
    rudp_bench::write_row(std::cout, row);
  }
  return 0;
}
```

- [ ] **Step 2: Update `harness/CMakeLists.txt`**

```cmake
add_library(rudp_bench_harness STATIC
  adapter_registry.cc
  scenario.cc
  metrics.cc
  csv_writer.cc
  proc_sampler.cc
  runner.cc
)
target_include_directories(rudp_bench_harness PUBLIC ${CMAKE_SOURCE_DIR})

add_executable(rudp-bench main.cc)
target_link_libraries(rudp-bench PRIVATE
  rudp_bench_harness
  adapter_raw_udp
  adapter_mini_rudp
)
```

`main.cc` で明示的に各 `register_*_adapter()` を呼んでいるため、`--whole-archive` 等の特殊リンクは不要。

- [ ] **Step 3: Build and run a one-shot scenario manually**

Run:
```bash
cmake --build build -j
./build/harness/rudp-bench --library=raw_udp --role=server --port=29000 --duration=2 --out=/tmp/s.csv &
sleep 0.1
./build/harness/rudp-bench --library=raw_udp --role=client --host=127.0.0.1 --port=29000 \
    --reliable=u --size=64 --conns=1 --rate=100 --duration=2 --out=/tmp/c.csv
wait
cat /tmp/c.csv
```

Expected: client CSV に `raw_udp,off,1,u,64,1,100,...` の行が出る。delivered > 0。

- [ ] **Step 4: Commit**

```bash
git add harness/main.cc harness/CMakeLists.txt
git commit -m "feat(harness): add rudp-bench CLI entrypoint"
```

---

## Task 13: tc qdisc helper script

**Files:**
- Create: `scripts/set_loss.sh`

`tc` は sudo 必須。スクリプトは loopback (`lo`) に `netem` を出し入れする。

- [ ] **Step 1: Write `scripts/set_loss.sh`**

```bash
#!/usr/bin/env bash
# Usage:
#   sudo scripts/set_loss.sh apply <pct>   # netem loss を lo に適用 (例: 5)
#   sudo scripts/set_loss.sh clear         # qdisc を削除
set -euo pipefail

cmd=${1:-}
case "$cmd" in
  apply)
    pct=${2:-0}
    tc qdisc del dev lo root 2>/dev/null || true
    if [ "$pct" != "0" ]; then
      tc qdisc add dev lo root netem loss "${pct}%"
    fi
    tc qdisc show dev lo
    ;;
  clear)
    tc qdisc del dev lo root 2>/dev/null || true
    tc qdisc show dev lo
    ;;
  *)
    echo "usage: $0 apply <pct> | clear" >&2
    exit 2
    ;;
esac
```

- [ ] **Step 2: Make executable**

Run: `chmod +x scripts/set_loss.sh`

- [ ] **Step 3: Verify (requires sudo, optional)**

Run (手動): `sudo scripts/set_loss.sh apply 0 && sudo scripts/set_loss.sh clear`
Expected: `tc qdisc show dev lo` 出力にエラーなし

- [ ] **Step 4: Commit**

```bash
git add scripts/set_loss.sh
git commit -m "feat(scripts): add tc qdisc loss injection helper"
```

---

## Task 14: Phase 1 sweep runner script

**Files:**
- Create: `scripts/run_phase1.sh`

bash の `for` で 32 シナリオ × 指定ライブラリを総当たり。各ランは `--out` でテンポラリ CSV に書かせ、ヘッダ除去して `results/phase1.csv` に追記。

- [ ] **Step 1: Write `scripts/run_phase1.sh`**

```bash
#!/usr/bin/env bash
# Phase 1 sweep runner.
# Usage:
#   scripts/run_phase1.sh --libraries=raw_udp,mini_rudp [--build-dir=build] [--results=results/phase1.csv]
#
# 注意: --loss > 0 の組合せは sudo で tc qdisc を操作するため、--loss-injection を与えたとき
# のみ実際に netem を適用する。デフォルトはメタデータのみ書き込み(注入なし)。

set -euo pipefail

LIBS="raw_udp,mini_rudp"
BUILD_DIR="build"
RESULTS="results/phase1.csv"
LOSS_INJECT="0"   # 0=skip, 1=apply via sudo

for arg in "$@"; do
  case "$arg" in
    --libraries=*) LIBS="${arg#*=}" ;;
    --build-dir=*) BUILD_DIR="${arg#*=}" ;;
    --results=*) RESULTS="${arg#*=}" ;;
    --loss-injection) LOSS_INJECT=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

BIN="$BUILD_DIR/harness/rudp-bench"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 2; }

mkdir -p "$(dirname "$RESULTS")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; if [ "$LOSS_INJECT" = "1" ]; then sudo scripts/set_loss.sh clear >/dev/null 2>&1 || true; fi' EXIT

# CSV header (1回だけ)
"$BIN" --library=raw_udp --role=server --port=29999 --duration=0 --out="$TMP/hdr.csv" 2>/dev/null || true
head -n1 "$TMP/hdr.csv" > "$RESULTS" 2>/dev/null || \
  echo "library,encryption,phase,reliable,size,conns,rate,loss,throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s" > "$RESULTS"

PORT_BASE=30000
PORT=$PORT_BASE

for lib in ${LIBS//,/ }; do
  for reliable in r u; do
    for size in 64 65536; do
      for conns in 1 1000; do
        for rate in 100 100000; do   # 低 / 飽和超え近似(後でライブラリ別調整)
          for loss in 0 5; do
            PORT=$((PORT + 1))
            if [ "$LOSS_INJECT" = "1" ]; then
              sudo scripts/set_loss.sh apply "$loss" >/dev/null
            fi

            S_OUT="$TMP/s_${lib}_${reliable}_${size}_${conns}_${rate}_${loss}.csv"
            C_OUT="$TMP/c_${lib}_${reliable}_${size}_${conns}_${rate}_${loss}.csv"

            timeout 90s "$BIN" --library="$lib" --role=server --port="$PORT" \
              --reliable="$reliable" --duration=30 --warmup=2 --loss="$loss" \
              --out="$S_OUT" &
            SPID=$!
            sleep 0.2

            timeout 90s "$BIN" --library="$lib" --role=client \
              --host=127.0.0.1 --port="$PORT" \
              --reliable="$reliable" --size="$size" --conns="$conns" --rate="$rate" \
              --duration=30 --warmup=2 --loss="$loss" \
              --out="$C_OUT" || true

            kill "$SPID" 2>/dev/null || true
            wait "$SPID" 2>/dev/null || true

            # client 行のみ抽出して追記(server 行は CPU/RSS 別計測なので別ファイル)
            if [ -f "$C_OUT" ]; then
              tail -n +2 "$C_OUT" >> "$RESULTS"
            fi

            if [ "$LOSS_INJECT" = "1" ]; then
              sudo scripts/set_loss.sh clear >/dev/null
            fi
          done
        done
      done
    done
  done
done

echo "wrote $RESULTS"
wc -l "$RESULTS"
```

- [ ] **Step 2: Make executable**

Run: `chmod +x scripts/run_phase1.sh`

- [ ] **Step 3: Run a tiny dry sweep with raw_udp only and short duration**

ローカル検証用に duration を一時的に短くして走らせる。`scripts/run_phase1.sh` 内の `--duration=30` を `--duration=2` に置換した一時版を `/tmp/phase1_smoke.sh` にコピーして実行(本物は変更しない):

```bash
sed 's/--duration=30/--duration=2/g' scripts/run_phase1.sh > /tmp/phase1_smoke.sh
chmod +x /tmp/phase1_smoke.sh
/tmp/phase1_smoke.sh --libraries=raw_udp --results=/tmp/phase1_smoke.csv
wc -l /tmp/phase1_smoke.csv
```

Expected: 32 scenario の試行、`/tmp/phase1_smoke.csv` に少なくとも 1 行以上(reliable シナリオは `na` 行)。

- [ ] **Step 4: Commit**

```bash
git add scripts/run_phase1.sh
git commit -m "feat(scripts): add phase1 sweep runner over scenario matrix"
```

---

## Task 15: Python phase1 table generator

**Files:**
- Create: `scripts/plot.py`
- Create: `scripts/requirements.txt`

`pandas` で `phase1.csv` を読み、ライブラリ × シナリオキーのピボットを `phase1_table.md` に書き出す。Phase 2 用のプロット雛形も同ファイルに含める(まだ呼ばれない)。

- [ ] **Step 1: Write `scripts/requirements.txt`**

```
pandas>=2.0
matplotlib>=3.7
```

- [ ] **Step 2: Write `scripts/plot.py`**

```python
#!/usr/bin/env python3
"""Phase 1 / Phase 2 result post-processing.

Usage:
    plot.py phase1-table --in results/phase1.csv --out results/phase1_table.md
    plot.py phase2-plot  --in results/phase2/<axis>.csv --out results/phase2/plots/
"""
import argparse
import sys
from pathlib import Path

import pandas as pd


def phase1_table(args: argparse.Namespace) -> int:
    df = pd.read_csv(args.in_path)
    df["scenario"] = (
        df["reliable"].astype(str)
        + "/" + df["size"].astype(str)
        + "/" + df["conns"].astype(str)
        + "/" + df["rate"].astype(str)
        + "/" + df["loss"].astype(str).str.rstrip("0").str.rstrip(".")
    )
    pivot_throughput = df.pivot_table(
        index="scenario", columns="library", values="throughput_mbps", aggfunc="mean"
    )
    pivot_delivery = df.pivot_table(
        index="scenario", columns="library", values="delivery_ratio", aggfunc="mean"
    )
    out = Path(args.out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        f.write("# Phase 1 results\n\n")
        f.write("## throughput (Mbps)\n\n")
        f.write(pivot_throughput.to_markdown())
        f.write("\n\n## delivery_ratio\n\n")
        f.write(pivot_delivery.to_markdown())
        f.write("\n")
    print(f"wrote {out}")
    return 0


def phase2_plot(args: argparse.Namespace) -> int:
    import matplotlib.pyplot as plt
    df = pd.read_csv(args.in_path)
    out_dir = Path(args.out_path)
    out_dir.mkdir(parents=True, exist_ok=True)
    axis_col = args.axis  # 例: "loss", "rate", "size", "conns"
    for metric in ["throughput_mbps", "delivery_ratio", "rtt_p50_us"]:
        fig, ax = plt.subplots(figsize=(8, 5))
        for lib, sub in df.groupby("library"):
            sub = sub.sort_values(axis_col)
            ax.plot(sub[axis_col], sub[metric], marker="o", label=lib)
        ax.set_xlabel(axis_col)
        ax.set_ylabel(metric)
        ax.set_title(f"{metric} vs {axis_col}")
        ax.legend()
        ax.grid(True, alpha=0.3)
        path = out_dir / f"{axis_col}_{metric}.png"
        fig.savefig(path, dpi=120, bbox_inches="tight")
        plt.close(fig)
        print(f"wrote {path}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    p1 = sub.add_parser("phase1-table")
    p1.add_argument("--in", dest="in_path", required=True)
    p1.add_argument("--out", dest="out_path", required=True)
    p1.set_defaults(func=phase1_table)
    p2 = sub.add_parser("phase2-plot")
    p2.add_argument("--in", dest="in_path", required=True)
    p2.add_argument("--out", dest="out_path", required=True)
    p2.add_argument("--axis", required=True, choices=["loss", "rate", "size", "conns"])
    p2.set_defaults(func=phase2_plot)
    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Make executable**

Run: `chmod +x scripts/plot.py`

- [ ] **Step 4: Smoke test against the dry sweep CSV from Task 14**

Run:
```bash
python3 -m pip install --user -r scripts/requirements.txt
python3 scripts/plot.py phase1-table --in /tmp/phase1_smoke.csv --out /tmp/phase1_smoke_table.md
head /tmp/phase1_smoke_table.md
```

Expected: markdown ピボット表が出力される

- [ ] **Step 5: Commit**

```bash
git add scripts/plot.py scripts/requirements.txt
git commit -m "feat(scripts): add phase1 table generator and phase2 plot skeleton"
```

---

## Task 16: End-to-end Phase 1 dry run (raw_udp + mini_rudp, 短時間版)

**Files:** (検証のみ、新規ファイルなし)

- [ ] **Step 1: Build clean**

Run: `rm -rf build && cmake -S . -B build && cmake --build build -j`
Expected: 全ターゲットビルド成功

- [ ] **Step 2: Run unit + smoke tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: 全テスト PASS

- [ ] **Step 3: Run short Phase 1 sweep against 2 baselines**

Run:
```bash
sed 's/--duration=30/--duration=3/g' scripts/run_phase1.sh > /tmp/phase1_short.sh
chmod +x /tmp/phase1_short.sh
/tmp/phase1_short.sh --libraries=raw_udp,mini_rudp --results=/tmp/phase1_short.csv
wc -l /tmp/phase1_short.csv
python3 scripts/plot.py phase1-table --in /tmp/phase1_short.csv --out /tmp/phase1_short.md
cat /tmp/phase1_short.md
```

Expected:
- `wc -l` で 1 (header) + α 行
- `phase1_short.md` に raw_udp / mini_rudp の throughput と delivery の表が出る
- raw_udp の reliable シナリオは `na` 行のみ
- delivery_ratio は ロス注入なし(`--loss-injection` 未指定)で 1.0 近辺

- [ ] **Step 4: Note any anomalies in commit message**

異常があれば issue 起票 / プラン2 のスコープに含める。なければ次へ。

- [ ] **Step 5: Commit (no source changes)**

```bash
git commit --allow-empty -m "chore: phase1 dry run validated for raw_udp + mini_rudp"
```

---

## Task 17: README expansion + plan completion note

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Expand README.md**

```markdown
# rudp-bench

Cross-library benchmark harness for reliable UDP / RUDP / QUIC implementations.
See `docs/superpowers/specs/2026-04-28-rudp-bench-design.md` for the full design.

## Status

Plan 1 complete: harness + raw_udp + mini_rudp baselines.
Subsequent plans add ENet, KCP, SLikeNet, UDT4, yojimbo, GNS, msquic, LiteNetLib adapters.

## Build

cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure

## Run a single scenario

./build/harness/rudp-bench --library=raw_udp --role=server --port=29000 --duration=10 --out=/tmp/s.csv &
./build/harness/rudp-bench --library=raw_udp --role=client --host=127.0.0.1 --port=29000 \
    --reliable=u --size=64 --conns=1 --rate=100 --duration=10 --out=/tmp/c.csv

## Phase 1 sweep

scripts/run_phase1.sh --libraries=raw_udp,mini_rudp --results=results/phase1.csv

# with tc loss injection (requires sudo)
scripts/run_phase1.sh --libraries=raw_udp,mini_rudp --results=results/phase1.csv --loss-injection

python3 scripts/plot.py phase1-table --in results/phase1.csv --out results/phase1_table.md
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: expand README with build / run / sweep instructions"
```

---

## Self-Review Checklist (do not skip)

After completing all tasks, run through:

1. **Spec coverage:** does each spec section have at least one task that materializes it?
   - 環境(loopback / tc): Task 13 (set_loss.sh) ✓
   - 計測指標(throughput/RTT/delivery/CPU/RSS/connect): Tasks 5, 7, 11 ✓
   - シナリオ軸(Phase 1 値): Task 14 (run_phase1.sh の for ループ) ✓
   - CSV フォーマット: Tasks 6, 12 ✓
   - 出力(phase1_table.md): Task 15 ✓
   - エラーハンドリング(failed/na/タイムアウト): Task 12 (na), Task 14 (timeout) ✓
   - テスト戦略(smoke per adapter): Tasks 8, 9 ✓
2. **Placeholder scan:** すべてのコードブロックに完全な実装が入っているか
3. **Type consistency:** `Adapter` IF, `ScenarioConfig`, `CsvRow` の名前/シグネチャは Tasks 2, 4, 6 で定義し以後一貫している ✓

スコープ外 (Plan 2 以降):
- ENet / SLikeNet / KCP / GNS / yojimbo / UDT4 / msquic adapter
- LiteNetLib (.NET 独立バイナリ)
- Phase 2 詳細スイープ
- Phase 1 sweep の rate を ライブラリ毎の飽和点に合わせる調整(現状は 100 / 100000 固定)
