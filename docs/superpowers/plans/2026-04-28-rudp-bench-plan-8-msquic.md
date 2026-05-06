# Plan 8: msquic アダプター

- 作成日: 2026-04-28
- ベースブランチ: master (Plan 9 = LiteNetLib アダプター実装済み)
- 実装ブランチ: feat/msquic
- 状態: 完了

## 目的

Microsoft の msquic (QUIC プロトコル実装) を rudp-bench ハーネスに接続する。
msquic は IETF QUIC (RFC 9000) の実装で、**TLS 1.3 による暗号化が必須**。
reliable=true は QUIC Stream、reliable=false は QUIC Datagram (RFC 9221) にマッピングする。

## ライブラリ概要

| 項目 | 内容 |
|---|---|
| ソース | https://github.com/microsoft/msquic |
| 固定タグ | v2.4.18 (release/2.4 ブランチ) |
| API | QUIC_API_TABLE (C 関数テーブル) |
| 暗号 | **ON (必須)** — TLS 1.3 (quictls バックエンド) |
| 依存 | libnuma-dev, openssl (cert 生成用) |
| ビルド形態 | 静的ライブラリ (QUIC_BUILD_SHARED=OFF) |

## 実装概要

### submodule

```
third_party/msquic  (v2.4.18 ピン)
```

`git submodule update --init --recursive` で quictls 等の内部依存も取得。

### ファイル構成

```
adapters/msquic/
  CMakeLists.txt        ← msquic 静的ビルド + adapter_msquic ライブラリ
  msquic_adapter.cc     ← Adapter 実装 (~430 行)
tests/
  test_msquic_smoke.cc  ← Capability + ReliableEcho + UnreliableEcho
```

### 主要設計判断

1. **TLS 証明書**: `std::call_once` で `openssl req -x509` によるセルフサイン証明書を `/tmp` に生成。ベンチマーク用途のショートカット。
2. **reliable=true → QUIC Stream**: 単方向ストリームを開き、4 バイト big-endian 長さプレフィクス + ペイロードを `QUIC_SEND_FLAG_FIN` で送信。受信側は `drain_frames()` でフレーミングを復元。
3. **reliable=false → QUIC Datagram**: `DatagramSend` / `DATAGRAM_RECEIVED` イベント (RFC 9221)。
4. **poll() は no-op**: msquic は内部スレッドで非同期イベント駆動。コールバックで `mutex` 経由の共有状態更新。
5. **IPv4 フォールバック**: コンテナ環境で IPv6 が利用不可の場合、msquic プラットフォーム層にパッチを当て AF_INET にフォールバック。
6. **ストリームカウント設定**: クライアント側にも `PeerBidiStreamCount` / `PeerUnidiStreamCount` を設定し、サーバーからクライアントへのストリーム作成を許可。

## テスト

- `test_msquic_smoke`: 3 テスト (Capability, ReliableEcho, UnreliableEcho)
- ポート: 0xC208 (reliable), 0xC209 (unreliable)
- タイムアウト: 10 秒 (TLS ハンドシェイクのオーバーヘッドを考慮)
