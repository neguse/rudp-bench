# RUDP Bench Plan 2: ENet Adapter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ENet (lsalzman/enet) を adapter として rudp-bench に追加し、Phase 1 スイープで raw_udp / mini_rudp と並んで計測可能にする。

**Architecture:** ENet を `third_party/enet/` に git submodule で取り込み、`adapters/enet/enet_adapter.cc` で `Adapter` 抽象を実装する。Plan 1 で確立した「明示 register 関数 + main.cc に1行追加」パターンを踏襲。ENet は async API なので `client_connect` は ENetPeer* を即返却、`is_connected` で CONNECT イベント受信済みフラグを確認する。スモークテスト1本を追加し、`run_phase1.sh` の `LIBS` デフォルトに `enet` を含める。

**Tech Stack:**
- ENet 1.3.18 以降(`lsalzman/enet`、git tag `v1.3.18`)
- 既存の C++17 / CMake (≥3.20) / GoogleTest 環境
- ENet API: `enet_host_create` / `enet_host_service` / `enet_peer_send` / `enet_packet_create`

---

## File Structure

```
rudp-bench/
├── .gitmodules                         # 新規 (Plan 2 で初登場)
├── third_party/
│   └── enet/                           # submodule (lsalzman/enet @ v1.3.18)
├── adapters/
│   └── enet/                           # 新規
│       ├── CMakeLists.txt
│       └── enet_adapter.cc
├── tests/
│   └── test_enet_smoke.cc              # 新規
├── CMakeLists.txt                      # add_subdirectory 追加
├── tests/CMakeLists.txt                # smoke test 登録
├── harness/CMakeLists.txt              # rudp-bench に adapter_enet をリンク
├── harness/main.cc                     # register_enet_adapter() 呼び出し
├── scripts/run_phase1.sh               # LIBS デフォルトに enet を追加
└── README.md                           # ENet ビルド手順を追記
```

責務:
- `third_party/enet/`: 上流 ENet ソースをそのまま submodule で取得。改変なし。
- `adapters/enet/CMakeLists.txt`: ENet を `add_subdirectory` で取り込み、`adapter_enet` STATIC ライブラリを定義。`enet` ターゲットをリンク。
- `adapters/enet/enet_adapter.cc`: `Adapter` IF を実装。サーバとクライアント両方で `ENetHost*` を保持、`ENetPeer*` ↔ `uint32_t conn_id` のマッピングを内部で管理。`recv` で受信した `ENetPacket` は payload を `buf` にコピーして `enet_packet_destroy`。
- `tests/test_enet_smoke.cc`: `mini_rudp` と同じパターンのエコー疎通テスト 1本 + capability テスト 1本。
- `scripts/run_phase1.sh`: `LIBS="raw_udp,mini_rudp,enet"` に変更。

---

## Conventions

Plan 1 と同じ:
- **TDD**: 失敗するテストを先に、最小実装で通し、必要ならリファクタ
- **コミット粒度**: タスク末尾で 1 コミット
- **Build**: `cmake -S . -B build && cmake --build build -j`
- **Test**: `ctest --test-dir build --output-on-failure`
- **コード規約**: C++17、`#pragma once`、`snake_case` 関数 / `PascalCase` 型、日本語コメント可

ENet 固有:
- ENet の C API を C++17 から呼ぶ際、`extern "C"` は ENet ヘッダ側で済んでいるので不要
- `enet_initialize()` はプロセス全体で1回。`atexit(enet_deinitialize)` を adapter 内 static init で登録する
- ENet は内部で reliable packet を自動フラグメンテーション/再アセンブリするため、size=65536 でも動作する(raw_udp/mini_rudp と異なる挙動。Phase 1 結果の比較で重要)

---

## Task 1: Feature ブランチと submodule 取り込み

**Files:**
- Create: `.gitmodules`
- Create: `third_party/` (submodule 取得で生成)
- Modify: `CMakeLists.txt`

- [ ] **Step 1: feature ブランチに切り替える**

```bash
git checkout -b feat/enet
```

- [ ] **Step 2: ENet を submodule として追加**

```bash
git submodule add https://github.com/lsalzman/enet.git third_party/enet
cd third_party/enet
git checkout v1.3.18
cd ../..
git add .gitmodules third_party/enet
```

- [ ] **Step 3: `CMakeLists.txt` に adapter サブディレクトリを追加(まだ作成前なのでビルドは失敗する想定)**

`CMakeLists.txt` の末尾、`add_subdirectory(adapters/mini_rudp)` の直後に追記:

```cmake
add_subdirectory(adapters/enet)
```

- [ ] **Step 4: ビルドが「ディレクトリが無い」で失敗することを確認**

Run: `cmake -S . -B build 2>&1 | tail -5`
Expected: `does not contain a CMakeLists.txt file` エラーで `adapters/enet` を指す

- [ ] **Step 5: コミット**

```bash
git add .gitmodules third_party/enet CMakeLists.txt
git commit -m "chore(enet): add enet submodule + cmake stub for adapter"
```

---

## Task 2: ENet adapter CMake 雛形

**Files:**
- Create: `adapters/enet/CMakeLists.txt`

- [ ] **Step 1: `adapters/enet/CMakeLists.txt` を書く**

ENet の上流 CMakeLists は `enet` という STATIC target を提供する。そのまま `add_subdirectory` で取り込んでリンクする。

```cmake
# ENet upstream provides STATIC `enet` target via its own CMakeLists.
# warnings はダウングレードする(上流コードに -Wall -Wextra -Wpedantic は厳しすぎる)
set(_saved_compile_options "${CMAKE_CXX_FLAGS}")
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/enet enet_build EXCLUDE_FROM_ALL)

# adapter wrapper
add_library(adapter_enet STATIC enet_adapter.cc)
target_link_libraries(adapter_enet PUBLIC rudp_bench_harness enet)
target_include_directories(adapter_enet PUBLIC ${CMAKE_SOURCE_DIR}/third_party/enet/include)
```

- [ ] **Step 2: 空の `enet_adapter.cc` を作成して configure を通す**

```bash
mkdir -p adapters/enet
touch adapters/enet/enet_adapter.cc
```

- [ ] **Step 3: configure + ビルドを試行**

Run: `cmake -S . -B build && cmake --build build -j --target adapter_enet`
Expected: `adapter_enet` ライブラリがビルドされる(空オブジェクトファイルになるが OK)

- [ ] **Step 4: コミット**

```bash
git add adapters/enet/CMakeLists.txt adapters/enet/enet_adapter.cc
git commit -m "build(enet): wire ENet upstream into cmake as adapter_enet"
```

---

## Task 3: 失敗するスモークテストを書く

**Files:**
- Create: `tests/test_enet_smoke.cc`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: `tests/test_enet_smoke.cc` を書く**

```cpp
#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <thread>

namespace rudp_bench { void register_enet_adapter(); }

class EnetRegistrar {
 public:
  EnetRegistrar() { rudp_bench::register_enet_adapter(); }
};
static EnetRegistrar registrar;

using namespace rudp_bench;

TEST(EnetSmoke, ReliableEcho) {
  auto server = create_adapter("enet");
  auto client = create_adapter("enet");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC102);

  std::thread server_thread([&]() {
    char buf[2048]; size_t len; uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        // poll once more to flush
        for (int i = 0; i < 10; ++i) {
          server->poll();
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC102);

  // is_connected が true になるまで poll
  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "enet-hello";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  char buf[2048]; size_t len; uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(len, sizeof(msg));
      EXPECT_STREQ(buf, msg);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(got);
  server_thread.join();
  client->close();
  server->close();
}

TEST(EnetSmoke, Capability) {
  auto a = create_adapter("enet");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "enet");
}
```

- [ ] **Step 2: `tests/CMakeLists.txt` に登録**

末尾に追記:

```cmake
add_executable(test_enet_smoke test_enet_smoke.cc)
target_link_libraries(test_enet_smoke PRIVATE adapter_enet GTest::gtest_main)
add_test(NAME test_enet_smoke COMMAND test_enet_smoke)
```

- [ ] **Step 3: ビルドして失敗することを確認(`register_enet_adapter` 未定義)**

Run: `cmake --build build -j --target test_enet_smoke 2>&1 | tail -20`
Expected: リンクエラー、`undefined reference to rudp_bench::register_enet_adapter()`

- [ ] **Step 4: コミット**

```bash
git add tests/test_enet_smoke.cc tests/CMakeLists.txt
git commit -m "test(enet): add failing smoke test for enet adapter"
```

---

## Task 4: ENet adapter の最小実装(Capability + 登録)

**Files:**
- Modify: `adapters/enet/enet_adapter.cc`

- [ ] **Step 1: `enet_adapter.cc` に骨組みを書く**

ENet の `enet_initialize` をプロセス全体で1回だけ呼ぶため、`std::call_once` を使う。

```cpp
#include "harness/adapter.h"
#include "harness/adapter_registry.h"

#include <enet/enet.h>

#include <cstring>
#include <mutex>

namespace {

void ensure_enet_init() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    if (enet_initialize() != 0) {
      std::abort();
    }
    std::atexit([]() { enet_deinitialize(); });
  });
}

class EnetAdapter : public rudp_bench::Adapter {
 public:
  EnetAdapter() { ensure_enet_init(); }
  ~EnetAdapter() override {
    if (host_) enet_host_destroy(host_);
  }

  void server_listen(uint16_t /*port*/) override {
    // Task 5 で実装
    std::abort();
  }
  uint32_t client_connect(const char* /*host*/, uint16_t /*port*/) override {
    std::abort();
  }
  bool is_connected(uint32_t /*conn_id*/) override { return false; }
  int send(uint32_t /*conn_id*/, const void* /*data*/, size_t /*len*/, bool /*reliable*/) override {
    return -1;
  }
  int recv(void* /*buf*/, size_t /*cap*/, size_t* /*out_len*/, uint32_t* /*out_conn_id*/) override {
    return 0;
  }
  void poll() override {}
  void close() override {
    if (host_) { enet_host_destroy(host_); host_ = nullptr; }
  }

  const char* name() const override { return "enet"; }
  bool supports(bool /*reliable*/) const override { return true; }
  bool encryption_on() const override { return false; }

 private:
  ENetHost* host_ = nullptr;
};

}  // namespace

namespace rudp_bench {
void register_enet_adapter() {
  register_adapter("enet",
      []() { return std::make_unique<EnetAdapter>(); });
}
}  // namespace rudp_bench
```

- [ ] **Step 2: ビルドして capability テストだけ通ることを確認**

Run: `cmake --build build -j --target test_enet_smoke && ctest --test-dir build -R EnetSmoke.Capability --output-on-failure`
Expected: `EnetSmoke.Capability` PASS、`EnetSmoke.ReliableEcho` は走らせていないので結果なし

- [ ] **Step 3: ReliableEcho が abort することを確認**

Run: `ctest --test-dir build -R EnetSmoke.ReliableEcho --output-on-failure 2>&1 | tail -10`
Expected: SIGABRT で失敗(`server_listen` の `std::abort()` が発火)

- [ ] **Step 4: コミット**

```bash
git add adapters/enet/enet_adapter.cc
git commit -m "feat(enet): add adapter skeleton + register function"
```

---

## Task 5: server_listen + recv + send 実装

**Files:**
- Modify: `adapters/enet/enet_adapter.cc`

ENet の Host は server / client 共通の構造で、`server_listen` では address を bind 済みの `enet_host_create` を呼ぶ。
受信は `enet_host_service(host, &event, 0)` を `poll` で回し、`ENET_EVENT_TYPE_RECEIVE` を内部キューに入れて `recv` で取り出す方式。

- [ ] **Step 1: 内部状態とヘルパを追加**

`EnetAdapter` クラスに以下のメンバを追加(クラス先頭の `private:` 直前にメンバ追加):

```cpp
 private:
  struct InboundMsg {
    uint32_t conn_id;
    std::vector<uint8_t> data;
  };

  ENetHost* host_ = nullptr;
  bool is_server_ = false;

  // peer ↔ conn_id マッピング(双方向)
  std::unordered_map<ENetPeer*, uint32_t> id_by_peer_;
  std::unordered_map<uint32_t, ENetPeer*> peer_by_id_;
  std::unordered_set<uint32_t> connected_ids_;
  uint32_t next_id_ = 1;

  // 受信メッセージのキュー
  std::deque<InboundMsg> inbox_;
```

include を追記:

```cpp
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>
```

- [ ] **Step 2: `server_listen` 実装**

```cpp
void server_listen(uint16_t port) override {
  ENetAddress addr{};
  addr.host = ENET_HOST_ANY;
  addr.port = port;
  // 最大ピア数 4096(Phase 1 では conns ≤ 1000、余裕)、2 channel、帯域無制限
  host_ = enet_host_create(&addr, 4096, 2, 0, 0);
  if (!host_) std::abort();
  is_server_ = true;
}
```

- [ ] **Step 3: `poll` 実装(イベントを inbox_ + connected_ids_ に変換)**

```cpp
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
        InboundMsg m;
        m.conn_id = id;
        m.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
        inbox_.push_back(std::move(m));
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
}
```

- [ ] **Step 4: `recv` 実装**

```cpp
int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
  if (inbox_.empty()) return 0;
  auto& m = inbox_.front();
  if (m.data.size() > cap) {
    // 切り詰めず破棄して err を返す。Phase 1 では cap=2048〜65536 想定
    inbox_.pop_front();
    return -1;
  }
  std::memcpy(buf, m.data.data(), m.data.size());
  *out_len = m.data.size();
  *out_conn_id = m.conn_id;
  inbox_.pop_front();
  return 1;
}
```

- [ ] **Step 5: `send` 実装**

```cpp
int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
  auto it = peer_by_id_.find(conn_id);
  if (it == peer_by_id_.end()) return -1;
  ENetPeer* peer = it->second;
  uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
  ENetPacket* pkt = enet_packet_create(data, len, flags);
  if (!pkt) return -1;
  if (enet_peer_send(peer, 0, pkt) != 0) {
    enet_packet_destroy(pkt);
    return -1;
  }
  // ENet は host_service / host_flush で実際の送信。即時 flush して loopback latency を最小化。
  enet_host_flush(host_);
  return 0;
}
```

- [ ] **Step 6: `is_connected` を仮実装(client 側はまだ未実装、現時点では server 側用に connected_ids_ を見る)**

```cpp
bool is_connected(uint32_t conn_id) override {
  return connected_ids_.count(conn_id) > 0;
}
```

- [ ] **Step 7: コミット**

```bash
git add adapters/enet/enet_adapter.cc
git commit -m "feat(enet): implement server side (listen/poll/recv/send)"
```

---

## Task 6: client_connect 実装 + ReliableEcho テストを通す

**Files:**
- Modify: `adapters/enet/enet_adapter.cc`

- [ ] **Step 1: `client_connect` を実装**

ENet クライアントは `enet_host_create(NULL, 1, 2, 0, 0)` で host を作り、`enet_host_connect` で peer を取る。CONNECT イベントは `poll` 経由で `connected_ids_` に入る。

```cpp
uint32_t client_connect(const char* host, uint16_t port) override {
  if (!host_) {
    host_ = enet_host_create(nullptr, 32, 2, 0, 0);
    if (!host_) std::abort();
    is_server_ = false;
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
```

- [ ] **Step 2: ReliableEcho テストを実行**

Run: `ctest --test-dir build -R EnetSmoke --output-on-failure`
Expected: `EnetSmoke.ReliableEcho` PASS、`EnetSmoke.Capability` PASS

もし FAIL する場合の典型原因:
- サーバ側で CONNECT 処理しても client 側の poll を回さないと client の `is_connected` は false のまま。テストは正しく client poll を回している
- `enet_host_flush` を呼んでも sleep がなさすぎると loopback でパケットが届く前に poll が抜ける可能性。テストの sleep は 2ms なので OK

- [ ] **Step 3: 全テスト走らせて regression なし確認**

Run: `ctest --test-dir build --output-on-failure`
Expected: 全 PASS(既存の raw_udp / mini_rudp / harness ユニットテストも含む)

- [ ] **Step 4: コミット**

```bash
git add adapters/enet/enet_adapter.cc
git commit -m "feat(enet): implement client_connect + pass smoke test"
```

---

## Task 7: main.cc とビルド配線で `--library=enet` を有効化

**Files:**
- Modify: `harness/main.cc`
- Modify: `harness/CMakeLists.txt`

- [ ] **Step 1: `harness/main.cc` に register 関数の forward declaration と呼び出しを追加**

`harness/main.cc:9-12` の `namespace rudp_bench { ... }` ブロックを以下に書き換え:

```cpp
namespace rudp_bench {
void register_raw_udp_adapter();
void register_mini_rudp_adapter();
void register_enet_adapter();
}  // namespace rudp_bench
```

`main` 冒頭の register 呼び出し直後(`register_mini_rudp_adapter();` の直後)に追記:

```cpp
  rudp_bench::register_enet_adapter();
```

- [ ] **Step 2: `harness/CMakeLists.txt` の `rudp-bench` 実行ファイルに `adapter_enet` をリンク**

`target_link_libraries(rudp-bench PRIVATE ...)` の中身に `adapter_enet` を追加:

```cmake
target_link_libraries(rudp-bench PRIVATE
  rudp_bench_harness
  adapter_raw_udp
  adapter_mini_rudp
  adapter_enet
)
```

- [ ] **Step 3: ビルドして CLI でも疎通確認**

Run: `cmake --build build -j --target rudp-bench`
Expected: ビルド成功

実機で短時間スイープを 1 シナリオだけ流して動作確認:

```bash
./build/harness/rudp-bench --library=enet --role=server --port=30100 \
  --reliable=r --duration=3 --warmup=1 --loss=0 --out=/tmp/s.csv &
SPID=$!
sleep 0.3
./build/harness/rudp-bench --library=enet --role=client \
  --host=127.0.0.1 --port=30100 \
  --reliable=r --size=64 --conns=1 --rate=100 \
  --duration=3 --warmup=1 --loss=0 --out=/tmp/c.csv
wait $SPID
cat /tmp/c.csv
```

Expected: `library=enet,encryption=off,...` で `delivered>=200,sent>=300` 程度のレコードが書き出される(rate=100/s × 3秒、warmup 1 秒除外なので約200)

- [ ] **Step 4: コミット**

```bash
git add harness/main.cc harness/CMakeLists.txt
git commit -m "feat(enet): wire enet adapter into rudp-bench CLI"
```

---

## Task 8: run_phase1.sh と README 更新

**Files:**
- Modify: `scripts/run_phase1.sh`
- Modify: `README.md`

- [ ] **Step 1: `scripts/run_phase1.sh` の `LIBS` デフォルトに `enet` を追加**

`LIBS="raw_udp,mini_rudp"` の行を以下に変更:

```bash
LIBS="raw_udp,mini_rudp,enet"
```

- [ ] **Step 2: ENet の submodule 取得手順を README に追記**

`README.md` のビルド手順セクションに、最初の `cmake -S . -B build` の前に追記:

```markdown
## サブモジュール取得

ENet 等の third_party ライブラリは git submodule で管理しています。
clone 後は以下を実行してください:

\`\`\`bash
git submodule update --init --recursive
\`\`\`
```

(バッククォートは実際の README ではエスケープしないで書く)

サポート libraries 一覧に `enet` を追加(該当箇所がある場合のみ。なければ Task 1 の説明に含める)。

- [ ] **Step 3: 短時間スイープでビン詰め確認(loss 注入なし)**

```bash
scripts/run_phase1.sh --libraries=enet --results=/tmp/phase1_enet.csv
wc -l /tmp/phase1_enet.csv
head -5 /tmp/phase1_enet.csv
```

Expected: ヘッダ1行 + reliable/unreliable × size 2 × conns 2 × rate 2 × loss 2 = 32 行(ENet は両モード対応なので `na` は出ない)。実行時間は 1シナリオ約 60〜90秒 × 32 ≈ 30分程度。実行前にユーザに「30分かかるけど走らせるか」確認することを推奨。

(注: このステップは長時間ランなので、実行は手動確認後とし、計画上は 1 シナリオだけのサンプル `--libraries=enet` の限定スイープで CSV 行が 1 行以上書き出されることを確認すれば OK とする)

短縮確認:

```bash
# duration を短縮した dry run を main.cc を介して直接1シナリオだけ
./build/harness/rudp-bench --library=enet --role=server --port=31000 \
  --reliable=r --duration=2 --warmup=0 --loss=0 --out=/tmp/se.csv &
SPID=$!
sleep 0.2
./build/harness/rudp-bench --library=enet --role=client \
  --host=127.0.0.1 --port=31000 \
  --reliable=r --size=1024 --conns=10 --rate=200 \
  --duration=2 --warmup=0 --loss=0 --out=/tmp/ce.csv
wait $SPID
grep "library" /tmp/ce.csv
grep "enet" /tmp/ce.csv
```

Expected: CSV ヘッダと enet 行が両方出力されること

- [ ] **Step 4: コミット**

```bash
git add scripts/run_phase1.sh README.md
git commit -m "chore(enet): include enet in default phase1 sweep + readme update"
```

---

## Task 9: 最終レビュー + master へのマージ

- [ ] **Step 1: feature ブランチでフルテストとビルドを通す**

Run:
```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Expected: 全テスト PASS

- [ ] **Step 2: code-reviewer agent でレビューを依頼**

requesting-code-review skill を起動するか、code-reviewer agent を直接呼ぶ:

> "Plan 2 (ENet adapter) の実装を、`docs/superpowers/plans/2026-04-28-rudp-bench-plan-2-enet.md` の plan と照らしてレビューしてください。特に:
> - Adapter 抽象 IF の実装漏れ・規約違反
> - Plan 1 で確立した登録パターン (`register_X_adapter` + 明示展開) との整合
> - ENet 固有の resource leak (`enet_packet_destroy`、`enet_host_destroy`) の漏れ
> - スモークテストのカバレッジ"

- [ ] **Step 3: 指摘事項を修正してコミット**

レビュー結果に応じて 1〜数コミットで修正。critical 指摘がなければスキップ可。

- [ ] **Step 4: master にマージ(`--no-ff`)**

```bash
git checkout master
git merge --no-ff feat/enet -m "Merge Plan 2: ENet adapter"
```

- [ ] **Step 5: メモリ更新**

`/home/neguse/.claude/projects/-home-neguse-ghq-github-com-neguse-rudp-bench/memory/project_status.md` の Plan 2 行を `✅ 完了 (YYYY-MM-DD)` に更新する。

---

## 完了基準

- `ctest` で `test_enet_smoke` の両 case が PASS
- `./build/harness/rudp-bench --library=enet ...` で reliable / unreliable それぞれ CSV 1 行を吐ける
- `scripts/run_phase1.sh --libraries=enet` で 32 シナリオが完走し CSV ファイルに 32 行追記される(時間がかかるため手動確認可)
- master ブランチに `feat/enet` がマージ済み

## 留意事項

- ENet は reliable で内部フラグメンテーションするため `size=65536` でも `mini_rudp` のように EMSGSIZE で失敗しない。Phase 1 結果で `mini_rudp` と差が出るのは正常な挙動の差。memory の `phase2_backlog.md` 項目1 に該当
- ENet の `enet_host_service(host, &ev, 0)` は timeout 0 でノンブロック。CPU 100% のビジーループにならないよう、`runner.cc` 側のスリープ間隔(Plan 1 で確立済み)に頼る
- reliable/unreliable の切替は `enet_packet_create` の flags のみ。ordered/unsequenced のセマンティクス差は Phase 2 までは扱わない
