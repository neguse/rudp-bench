# Plan 4: SLikeNet アダプタ実装

- 作成日: 2026-04-28
- ブランチ: feat/slikenet
- 状態: 実装完了・PR 作成済み

## 目的

ENet (Plan 2) の構造を鏡にして SLikeNet (RakNet OSS フォーク) アダプタを追加する。
暗号化は OFF (`encryption_on()` = false)。reliable / unreliable の両モードをサポートする。

## ライブラリ情報

| 項目 | 値 |
|---|---|
| リポジトリ | https://github.com/SLikeSoft/SLikeNet |
| 固定タグ | v.0.1.3 |
| submodule パス | `third_party/slikenet` |
| CMake ターゲット | `RakNetLibStatic` |
| ヘッダパス | `Source/include/slikenet/` |
| 名前空間 | `SLNet::` |
| 暗号化 | OFF (InitializeSecurity を呼ばない) |

## 設計方針

### クラス構成

`SLikeNetAdapter` は `rudp_bench::Adapter` を実装し、
`SLNet::RakPeerInterface*` を 1 インスタンス保持する(サーバ／クライアント共通)。

### 初期化

- **サーバ**: `server_listen(port)` で `Startup(4096, &sd, 1)` + `SetMaximumIncomingConnections(4096)`
- **クライアント**: 初回 `client_connect()` で `Startup(4096, &sd_port0, 1)` + `Connect(host, port, nullptr, 0)`

### ユーザデータのパケット識別

SLikeNet は `Receive()` が返す `Packet::data[0]` をメッセージ識別子として使う。
ユーザデータとシステムメッセージを区別するため:

- **送信時**: データ先頭に `ID_USER_PACKET_ENUM` (1 byte) を付加
- **受信時**: `data[0] == ID_USER_PACKET_ENUM` のときのみ `data[1..]` をペイロードとして deliver

### コネクション ID マッピング

- `SLNet::RakNetGUID` の `g` フィールド (`uint64_t`) を内部キーとして使う
- サーバ側: `ID_NEW_INCOMING_CONNECTION` を受けたとき `register_guid()` で新 conn_id 払い出し
- クライアント側: `client_connect()` 発行時に `pending_ids_` キューに積み、
  `ID_CONNECTION_REQUEST_ACCEPTED` を受けたとき一括 resolve
  (同一サーバへの複数 `Connect()` は同一物理コネクションを共有する SLikeNet の仕様に準拠)

### マルチ接続

`conns > 1` の場合、同一サーバへの複数 `Connect()` 呼び出しはすべて同一 RakNet 物理コネクションにマップされる。
benchmark では全 conn_id が同一 GUID を指すため、送受信は問題なく動作する。
スループット計測は単一物理コネクションの帯域に制限されるが、これは SLikeNet のアーキテクチャ上の特性として記録する。

### コンパイル警告の抑制

SLikeNet は古い RakNet コードを多数含み、グローバルの `-Wall -Wextra -Wpedantic` が伝播すると
大量の警告が出る。`adapters/slikenet/CMakeLists.txt` にて:

```cmake
target_compile_options(RakNetLibStatic PRIVATE -w)
```

を追加して上流コードの警告を完全抑制している。

## 実装ファイル

| ファイル | 内容 |
|---|---|
| `third_party/slikenet` | git submodule (v.0.1.3) |
| `adapters/slikenet/CMakeLists.txt` | upstream add_subdirectory + adapter_slikenet ライブラリ定義 |
| `adapters/slikenet/slikenet_adapter.cc` | `SLikeNetAdapter` 実装 + `register_slikenet_adapter()` |
| `tests/test_slikenet_smoke.cc` | 疎通テスト (port 0xC104 = 49412) |

## ハーネス接続

- `CMakeLists.txt`: `add_subdirectory(adapters/slikenet)`
- `harness/CMakeLists.txt`: `rudp-bench` に `adapter_slikenet` をリンク
- `harness/main.cc`: `register_slikenet_adapter()` 呼び出しを追加
- `tests/CMakeLists.txt`: `test_slikenet_smoke` を追加
- `scripts/run_phase1.sh`: `LIBS` に `slikenet` を追加

## テスト結果

| テスト | 結果 |
|---|---|
| SLikeNetSmoke.ReliableEcho | PASS |
| SLikeNetSmoke.Capability | PASS |

## 既知の制限・偏差

1. **多接続**: `conns > 1` のシナリオでは複数の conn_id が単一 RakNet コネクションを共有する。
   ENet と異なり、SLikeNet は同一アドレスへの多重コネクションをプロトコル上サポートしない。
2. **ビルド時間**: SLikeNet / RakNet のソースファイル数が多いため、初回ビルドは 3〜5 分かかる。
3. **スレッドモデル**: SLikeNet は内部スレッドを起動する。`poll()` は `Receive()` を呼ぶだけで
   スレッド間同期は SLikeNet 内部で処理される。

## 未解決

- なし
