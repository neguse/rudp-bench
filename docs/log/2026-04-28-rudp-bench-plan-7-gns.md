# Plan 7: GNS (GameNetworkingSockets) アダプター

- 作成日: 2026-04-28
- ベースブランチ: master (Plan 2 = ENet アダプター実装済み)
- 実装ブランチ: feat/gns
- 状態: 完了

## 目的

Valve Software の GameNetworkingSockets (GNS) ライブラリを rudp-bench ハーネスに接続する。
GNS は Steam 由来の RUDP 実装で、**暗号化がデフォルト ON** という他の実装にはない特性を持つ。
Plan 2 (ENet) のファイル構成・テスト構造を canonical として踏襲する。

## ライブラリ概要

| 項目 | 内容 |
|---|---|
| ソース | https://github.com/ValveSoftware/GameNetworkingSockets |
| 固定タグ | v1.4.1 |
| API | ISteamNetworkingSockets (仮想関数インタフェース) |
| 暗号 | **ON (デフォルト)** — AES/ChaCha20 + X25519 鍵交換 |
| 依存 | protobuf-compiler, libprotobuf-dev, libssl-dev |
| ビルド形態 | 静的ライブラリ `GameNetworkingSockets_s` |

## 実装概要

### submodule

```
third_party/gns  (v1.4.1 ピン)
```

GNS 内部の WebRTC submodule (P2P ICE 用) は googlesource.com へのアクセスが制限されているため
取得できないが、本実装では `ENABLE_ICE=OFF` としてビルドするため不要。

### CMake オプション (adapters/gns/CMakeLists.txt)

```cmake
BUILD_STATIC_LIB=ON   # 静的ライブラリのみ
BUILD_SHARED_LIB=OFF
ENABLE_ICE=OFF        # ローカル IP 接続のみ。ICE(P2P穿孔) は不要
BUILD_EXAMPLES=OFF
BUILD_TESTS=OFF
BUILD_TOOLS=OFF
```

ICE を無効化することで abseil/WebRTC を除いた軽量ビルドになる。

### アダプターアーキテクチャ

GNS はシングルトン API (`SteamNetworkingSockets()`) を通じて全接続を管理する。
複数のアダプターインスタンスが共存できるよう、グローバルレジストリ (`GnsGlobal`) で
`HSteamListenSocket → GnsAdapter*` と `HSteamNetConnection → GnsAdapter*` のマッピングを保持する。

コールバック (`gns_status_callback`) はグローバルレジストリを参照してアダプターを特定し、
`g_gns.mtx` を解放してから `adapter->on_status_changed()` を呼ぶことでデッドロックを回避する。

クライアント側の接続は `ConnectByIPAddress()` の直後にレジストリへ登録することで、
`Connecting` コールバック内での `AcceptConnection()` 誤発行を防ぐ。

### ファイル構成

```
adapters/gns/
├── CMakeLists.txt     # GNS upstream + adapter_gns static lib
└── gns_adapter.cc     # GnsAdapter クラス + グローバルレジストリ
tests/
└── test_gns_smoke.cc  # port 0xC107、Capability + ReliableEcho
```

### 変更ファイル

- `CMakeLists.txt` — `LANGUAGES C CXX` 追加、`adapters/gns` subdirectory
- `harness/CMakeLists.txt` — `adapter_gns` を `rudp-bench` にリンク
- `harness/main.cc` — `register_gns_adapter()` 呼び出し追加
- `tests/CMakeLists.txt` — `test_gns_smoke` テスト登録
- `scripts/run_phase1.sh` — LIBS に `gns` 追加
- `README.md` — ステータス更新

## ビルド依存インストール

実行環境にて以下を事前インストール:

```
apt-get install -y protobuf-compiler libprotobuf-dev libssl-dev
```

`libssl-dev` はすでに存在、`protobuf-*` をインストールして対応済み。

## テスト結果

```
10/10 Tests passed
  test_gns_smoke (Capability + ReliableEcho) — PASS (0.24 sec)
```

## ハーネス疎通確認

```
./build/harness/rudp-bench --library=gns --role=server --port=30207 --duration=5
./build/harness/rudp-bench --library=gns --role=client --host=127.0.0.1 --port=30207 \
    --reliable=r --size=64 --conns=1 --rate=100 --duration=5

# 結果 (client 行):
# library=gns, encryption=on, delivery_ratio=1.002, connect_ms=3
```

## 既知の特性と注意事項

- **GNS の接続確立コスト**: 暗号化ハンドシェイク (X25519 + AES) のため、ENet や raw_udp より
  connect_ms が高い。loopback 環境でも数 ms 必要。Phase 1 比較表では注記する。
- **delivery_ratio > 1.0**: warmup 期間の echo が計測区間に到着する既知アーティファクト
  (README 記載済みの全体共通問題)。
- **ICE 無効**: GNS の P2P 穿孔機能は本ベンチでは使わない。loopback IP 接続のみ。
- **WebRTC submodule 未取得**: `git submodule update --init --recursive` は WebRTC の
  取得でアクセス拒否になる。`ENABLE_ICE=OFF` で回避済み。
  `git submodule update --init third_party/enet third_party/gns` のみで十分。

## 未解決事項

- 現状なし
