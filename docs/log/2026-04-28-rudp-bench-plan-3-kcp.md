# Plan 3: KCP アダプタ実装

- 作成日: 2026-04-28
- 対象ライブラリ: [skywind3000/kcp](https://github.com/skywind3000/kcp) tag **1.7** (38e0c93)
- 状態: 実装済み・全テスト通過

## 目的

Plan 2 (ENet) と同構造で KCP アダプタを追加し、`rudp-bench` CLI で `--library=kcp` を指定できるようにする。

## ライブラリ概要

| 項目 | 内容 |
|---|---|
| 種別 | ARQ (Automatic Repeat reQuest) プロトコルライブラリ |
| 言語 | C (single-file: `ikcp.c` / `ikcp.h`) |
| ソケット | **含まない** – アプリ側が UDP ソケットを用意する必要がある |
| 暗号 | なし (`encryption_on()` → `false`) |
| 接続確立 | ハンドシェイクなし (conv ID で会話を区別) |
| ピン | tag 1.7 (2020-04-24) – 最後の安定リリース |

## 実装方針

### ソケット構成

`raw_udp` のソケット構成を踏襲する。

- **server**: 1 個の `SOCK_DGRAM` ソケットを `bind`、`recvfrom()` で全クライアントから受信。
- **client**: 1 個の `SOCK_DGRAM` ソケットを `connect()` (サーバ側 addr)、全コネクションで共有。複数コネクションは conv で識別。

### ワイヤフォーマット

先頭 1 バイトのプレフィックスで種別を区別する。

```
Reliable   (KCP ARQ): [0x01] | <KCP frame>
Unreliable (raw bypass): [0x00] | <conv:4B LE> | <app payload>
```

- **Reliable**: KCP output callback 内で `0x01` を先頭に付与して `sendto` / `send`。受信側は `ikcp_input()` に渡す。
- **Unreliable**: KCP を完全バイパスし `sendto` / `send` で直接送出。`conv` をヘッダに含め、サーバ・クライアント双方が接続を特定できるようにする。

### conv と conn_id の関係

- クライアント側: `conv = conn_id`（`client_connect()` が返す値と同値）。
- サーバ側: 受信パケットの `conv` と送信元アドレスの組 `(addr_key, conv)` で接続を識別し、サーバ独自の `conn_id` を払い出す。

### 接続確立

KCP はハンドシェイクを持たないため `client_connect()` 呼び出し時点で即座に `connected = true` とする。サーバ側は最初のパケット受信時に接続エントリを生成し `connected = true` とする。

### KCP 設定

```cpp
ikcp_nodelay(kcp, 1, 10, 2, 1);  // nodelay=1, interval=10ms, fastresend=2, nocwnd=1
ikcp_setmtu(kcp, 1400);
ikcp_wndsize(kcp, 128, 128);
```

`nodelay=1` (fastest mode) を採用し、低遅延テストシナリオに対応する。

### KCP タイマ更新

`poll()` 呼び出しごとに全 KCP インスタンスに対して `ikcp_update(now_ms)` を呼ぶ。`ikcp_check()` を使ったスケジュール最適化は Phase 2 バックログとする。

## コミットシーケンス

Plan 2 の 8 ステップ構成を踏襲する。

| # | コミット | 内容 |
|---|---|---|
| 1 | `chore: add kcp submodule` | `third_party/kcp` を tag 1.7 でピン |
| 2 | `build(cmake): wire kcp adapter into build + harness` | CMakeLists 全箇所 + `main.cc` 宣言 |
| 3 | `test(kcp): add test_kcp_smoke (failing)` | `test_kcp_smoke.cc` (ReliableEcho / UnreliableEcho / Capability) |
| 4 | `feat(kcp): skeleton adapter – types + register stub` | 型定義 + `register_kcp_adapter()` + no-op スタブ |
| 5 | `feat(kcp): server-side listen, alloc_server_conn, drain_socket (server path)` | サーバ受信パス全体 |
| 6 | `feat(kcp): client_connect + shared UDP socket + is_connected` | クライアント接続 |
| 7 | `feat(kcp): wire send() + full client drain_socket path` | `send()` + クライアント受信パス + 全テスト通過 |
| 8 | `chore: run_phase1.sh LIBS+=kcp + README status + known issues` | スクリプト・ドキュメント更新 |

## ファイル構成

```
adapters/kcp/
├── CMakeLists.txt        # add_subdirectory third_party/kcp + adapter_kcp STATIC
└── kcp_adapter.cc        # KcpAdapter 実装

tests/
└── test_kcp_smoke.cc     # ReliableEcho / UnreliableEcho / Capability (port 0xC103/0xC104)

docs/superpowers/plans/
└── 2026-04-28-rudp-bench-plan-3-kcp.md  # 本ドキュメント
```

変更ファイル:

| ファイル | 変更内容 |
|---|---|
| `CMakeLists.txt` | `add_subdirectory(adapters/kcp)` 追加 |
| `harness/CMakeLists.txt` | `target_link_libraries` に `adapter_kcp` 追加 |
| `harness/main.cc` | `register_kcp_adapter()` 宣言・呼び出し追加 |
| `tests/CMakeLists.txt` | `test_kcp_smoke` ターゲット追加 |
| `scripts/run_phase1.sh` | デフォルト `LIBS` に `kcp` 追加 |
| `README.md` | ステータス行・sweep 例・既知の挙動更新 |

## 検証結果

```
# cmake build
cmake -S . -B build && cmake --build build -j  → 成功 (警告のみ、エラーなし)

# ctest (全 10 テスト)
ctest --test-dir build --output-on-failure
  test_kcp_smoke ...................   Passed    0.07 sec
  (残 9 テストも全 Passed)
100% tests passed, 0 tests failed out of 10

# harness smoke (reliable)
./build/harness/rudp-bench --library=kcp --role=server --port=30203 \
    --reliable=r --duration=2 --warmup=0 --loss=0 --out=/tmp/s.csv &
sleep 0.3
./build/harness/rudp-bench --library=kcp --role=client --host=127.0.0.1 \
    --port=30203 --reliable=r --size=64 --conns=1 --rate=100 \
    --duration=2 --warmup=0 --loss=0 --out=/tmp/c.csv
→ delivery_ratio=1.0000, rtt_p50_us=10178 (> 0) ✓

# harness smoke (unreliable)
→ delivery_ratio=1.0000, rtt_p50_us=89 (> 0) ✓
```

## Plan 2 との差異

| 項目 | Plan 2 (ENet) | Plan 3 (KCP) |
|---|---|---|
| ソケット管理 | ENet 内部 (enet_host) | 自前 (raw_udp と同様) |
| 接続確立 | ENet CONNECT イベント | なし (即時 connected) |
| unreliable 実装 | ENet channel (ENET_PACKET_FLAG_RELIABLE なし) | raw sendto バイパス (PREFIX_RAW + conv header) |
| フラッシュタイミング | `poll()` 末尾 `enet_host_flush()` | KCP output callback 内で即時 `sendto`/`send` |
| 複数 conn の socket 共有 | ENet ホスト内で管理 | 単一 fd を全 conn で共有、conv で demux |

## 既知の制限 / Phase 2 バックログ

| 項目 | 詳細 |
|---|---|
| KCP タイマ粒度 | `ikcp_update` を `poll()` ごとに呼ぶが、`poll()` 呼び出し間隔に依存。`ikcp_check()` でスケジュール最適化すると高 conns 時の CPU が下がる可能性がある |
| reliable RTT 底上げ | loopback での reliable RTT は KCP 内部タイマ (10ms nodelay) が支配項になる。ENet / raw_udp より高い傾向。Phase 2 で `ikcp_update` 呼び出し頻度を軸にスイープする価値がある |
| 複数サーバ対象 | client の共有ソケットは `connect()` で 1 サーバアドレスに固定。Phase 1 の想定 (ローカル loopback 単一サーバ) では問題なし |
| unreliable conv ヘッダ | conv を 4B LE で埋め込む。KCP フレームの conv フィールドと同じエンディアンを使うことで実装を統一 |
