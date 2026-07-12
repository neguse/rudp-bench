# Plan 5: UDT4 adapter

- 作成日: 2026-04-28
- 状態: 完了

## フォーク選択

決定木を順番に試した結果:

1. `https://github.com/loonycyborg/UDT-fixed` → HTTP 404 (リポジトリ存在せず)
2. `https://github.com/eric-yhc/udt4` → HTTP 404 (リポジトリ存在せず)
3. git HTTPS clone / `git ls-remote` → "No such device or address" (ネットワーク不可)
4. **採用: system apt パッケージ `libudt-dev 4.11+dfsg1`**

apt パッケージが `/usr/lib/libudt.{a,so}` + `/usr/include/udt/udt.h` を提供するため、
`find_library(UDT_LIBRARY udt)` / `find_path(UDT_INCLUDE_DIR udt/udt.h)` で CMake から参照できる。
サブモジュールは不要。

## 実装判断

### フレーミング

UDT4 は `SOCK_STREAM over UDP` のためメッセージ境界を持たない。
adapter 内部で **4 バイト LE 長プレフィックス** を付与する:

```
[ len: uint32_t LE ][ payload: len bytes ]
```

送信: `send_all()` で header(4B) + payload をループ送信。  
受信: `ConnState::partial` バッファに蓄積し、フレームが揃ったら `inbox_` に push。

### unreliable 非対応

`supports(false)` が `false` を返す。harness の capability check が `na` 行を自動出力するため、
`--reliable=u` シナリオは計測しない。

### 非同期 I/O

- `UDT_RCVSYN = false` で recv を非ブロッキング化
- UDT 内蔵 epoll (`UDT::epoll_create / epoll_add_usock / epoll_wait`) で timeout=0 のノンブロッキングポール
- `UDT_SNDSYN = true`(デフォルト)のままでブロッキング送信 → `send_all` ループで部分送信対応

### UDT グローバル初期化

`UDT::startup()` / `UDT::cleanup()` を `std::once_flag` + `std::atexit` でプロセスに 1 回だけ呼ぶ
(`ensure_udt_init()`)。複数 adapter インスタンスを生成しても多重初期化しない。

## 実装ファイル一覧

| ファイル | 内容 |
|---|---|
| `adapters/udt4/CMakeLists.txt` | find_path/find_library でシステム libudt を参照 |
| `adapters/udt4/udt4_adapter.cc` | Udt4Adapter クラス: 長プレフィックスフレーミング、epoll ポーリング |
| `tests/test_udt4_smoke.cc` | ReliableEcho + Capability テスト (port 0xC105) |
| `harness/main.cc` | `register_udt4_adapter()` 追加 (register_slikenet の直後) |
| `harness/CMakeLists.txt` | `adapter_udt4` リンク追加 |
| `CMakeLists.txt` | `add_subdirectory(adapters/udt4)` 追加 |
| `tests/CMakeLists.txt` | `test_udt4_smoke` テスト追加 |
| `scripts/run_phase1.sh` | `LIBS` に `udt4` 追加 |
| `README.md` | ステータス行 + 既知の挙動エントリ追加 |

## テスト結果

- `ctest --test-dir build`: test_udt4_smoke (ReliableEcho + Capability) PASS
- `--reliable=u` シナリオ: `na` 行出力を確認 (`supports(false) == false`)
- `delivery_ratio > 0` を loopback で確認

## 既知の制限・注記

- apt パッケージ `libudt-dev` がインストールされていない環境ではビルドエラー
  (`find_library` / `find_path` の REQUIRED が失敗)。
  `sudo apt-get install libudt-dev` で解決する。
- UDT 4.11 のヘッダは deprecated dynamic-exception-spec を使っており `-Wdeprecated` 警告が出る。
  `target_compile_options(adapter_udt4 PRIVATE -Wno-deprecated)` で抑制済み。
- `partial` バッファは `std::vector::erase(begin, begin+n)` で先頭削除しており、
  多数のコネクションで大きなメッセージを受信し続けると O(n) コストが積み重なる。
  Phase 2 では `std::deque<uint8_t>` または ring buffer への置き換えを検討する。
