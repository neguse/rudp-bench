# Plan 6: yojimbo adapter

- 作成日: 2026-04-28
- ブランチ: feat/yojimbo
- 状態: 実装完了

## 概要

Glenn Fiedler の netcode + reliable + serialize + libsodium スタック (yojimbo v1.2.5) を rudp-bench に組み込む。
Plan 2 (ENet) と同じ構造でアダプタを実装した。

## ライブラリ概要

| 項目 | 値 |
|---|---|
| バージョン | v1.2.5 (タグ pinned) |
| 暗号 | ON / 必須 (libsodium AEAD) |
| トランスポート | UDP + netcode プロトコル (connect token 認証) |
| 信頼性 | reliable-ordered / unreliable-unordered (チャネル別) |
| 最大接続数 | 64 (yojimbo::MaxClients) |

## ビルド方式

**選択: (a) 直接 CMake — premake5 不要**

yojimbo は公式には premake5 を使用するが、本プランでは `adapters/yojimbo/CMakeLists.txt` に
premake5.lua の依存関係を直接移植し、premake5 をインストールなしでビルドできるようにした。

コンパイル対象:
- `source/*.cpp` — yojimbo C++ コア
- `netcode/netcode.c` — netcode プロトコル (UDP 上のセキュア接続確立)
- `reliable/reliable.c` — 信頼性レイヤ
- `tlsf/tlsf.c` — TLSF アロケータ

リンク: system libsodium (`libsodium-dev`), 上記 4 ターゲットの静的ライブラリ。

システム依存パッケージ:
```
sudo apt-get install libsodium-dev libmbedtls-dev
```

## 実装上の判断事項

### 1. InsecureConnect の採用

本番では認証サーバ (matchmaker) が connect token を発行するが、ベンチマークでは
`Client::InsecureConnect(privateKey, clientId, serverAddress)` を使用し、
loopback 内で完結する自己署名トークンで接続する。

ハードコードされたテストキー (`kTestPrivateKey`) はサーバ/クライアント共通。

### 2. サーバ bind アドレス

netcode プロトコルは connect token 内のサーバアドレスとサーバの bound address を照合する。
`0.0.0.0` にバインドすると whitelist チェックで拒否されるため、`127.0.0.1` に明示バインドする。
これは loopback 専用ベンチマークとして許容される制約。

### 3. 接続数上限

yojimbo のコンパイル時定数 `MaxClients = 64`。Phase 1 シナリオの `conns=1000` は
この上限を超えるため、yojimbo で 1000 同時接続シナリオは実行不可。
`conns > 64` のシナリオでは harness が -1 (send 失敗) を受けて低 delivery_ratio を記録する見込み。
Phase 2 で MaxClients を 1024 に拡張するか、または 64 コン接続シナリオに制限する予定。

### 4. ペイロードサイズ上限

yojimbo の BenchMessage は最大 4096 bytes。65536 bytes シナリオでは `min(len, 4096)` に
切り詰めて送信する。delivery_ratio のカウント(seq ベース)は影響しないが throughput は過小評価になる。

### 5. CPU オーバーヘッド

yojimbo は毎パケット libsodium による ChaCha20-Poly1305 暗号化を行う。
`raw_udp` / `mini_rudp` / `enet` と比べて CPU コストが高いことを設計仕様として許容。

### 6. InitializeYojimbo / ShutdownYojimbo

複数アダプタ インスタンスが同一プロセスに共存するため `std::once_flag` で初期化を 1 回に制限し、
`ShutdownYojimbo()` は `std::atexit` から呼ぶ。

## テスト

`tests/test_yojimbo_smoke.cc` (ポート 0xC106 = 49414):
- `YojimboSmoke.Capability` — `encryption_on() == true` を確認
- `YojimboSmoke.ReliableEcho` — reliable echo のラウンドトリップ確認

## 未解決 / Unresolved

1. **conns=1000 シナリオ**: `MaxClients = 64` の制約。`YOJIMBO_MAX_CLIENTS` を大きな値にして
   再コンパイルすれば緩和できるが、メモリ消費(~10MB/クライアント)が問題になりうる。

2. **非 loopback 接続**: `server_listen` は `127.0.0.1` にバインドするため、
   異なるホスト間のベンチには対応していない。

3. **65536 bytes シナリオ**: 4096 bytes に切り詰め。`BlockMessage` を使えば
   より大きなペイロードを reliable チャネルで送れるが、API が複雑になる。

## 成果物

| ファイル | 役割 |
|---|---|
| `third_party/yojimbo` | submodule (v1.2.5 pin) |
| `adapters/yojimbo/CMakeLists.txt` | 直接 CMake ビルド定義 |
| `adapters/yojimbo/yojimbo_adapter.cc` | adapter 実装 |
| `tests/test_yojimbo_smoke.cc` | smoke テスト |
