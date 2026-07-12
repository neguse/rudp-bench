# Plan 9: LiteNetLib (.NET) Adapter

- 作成日: 2026-04-28
- ブランチ: feat/litenetlib
- 状態: 実装完了

## 概要

LiteNetLib (RevenantX/LiteNetLib) アダプタを C# / .NET 8 の独立バイナリとして実装する。
これは他の C++ アダプタとは **アーキテクチャ上大きく異なる**:

- C++ harness バイナリ (`rudp-bench`) に組み込まず、`adapters/litenetlib/litenetlib_adapter` として独立
- NuGet 経由でライブラリを取得 (git submodule は不使用)
- CMake の `add_custom_target` から `dotnet build` を呼ぶことで `cmake --build` から両系統を統一ビルド
- テストは C++ GTest ではなく shell スクリプト (`tests/test_litenetlib_smoke.sh`) で実施

## 実装ファイル一覧

| ファイル | 内容 |
|---|---|
| `adapters/litenetlib/litenetlib_adapter.csproj` | .NET 8 プロジェクト; LiteNetLib 2.1.3 を NuGet 参照 |
| `adapters/litenetlib/Program.cs` | CLIパース・サーバ・クライアント・CSV出力の全実装 |
| `tests/test_litenetlib_smoke.sh` | loopback 疎通確認 shell テスト |
| `tests/CMakeLists.txt` | `test_litenetlib_smoke` を ctest に登録 |
| `CMakeLists.txt` | `litenetlib_build` custom target (`dotnet build -c Release`) を ALL に追加 |
| `scripts/run_phase1.sh` | `litenetlib` ライブラリを LIBS に追加; `.NET` バイナリへの dispatch 処理 |
| `.gitignore` | `adapters/litenetlib/bin/`, `adapters/litenetlib/obj/` を除外 |
| `README.md` | status 表更新; .NET 8 ビルド前提条件を記載 |

## アーキテクチャ上の決定事項

### 1. 独立バイナリ方式

C++ の `Adapter` 仮想 IF に組み込む代わりに、同一の CLI フラグ・CSV フォーマットを実装した独立バイナリとして動作させる。`scripts/run_phase1.sh` がライブラリ名で分岐し、`litenetlib` の場合は `.NET` バイナリを呼ぶ。

### 2. NuGet パッケージ取得

```
dotnet add package LiteNetLib   # LiteNetLib 2.1.3
```

`dotnet build` 時に自動的に restore されるため、オフライン環境では事前に `dotnet restore` が必要。

### 3. CMake ドライバ

```cmake
find_program(DOTNET_EXECUTABLE dotnet)
if(DOTNET_EXECUTABLE)
    add_custom_target(litenetlib_build ALL
        COMMAND ${DOTNET_EXECUTABLE} build -c Release
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/adapters/litenetlib"
    )
endif()
```

`dotnet` が見つからない場合はワーニングを出してスキップ。C++ 系統のビルドは継続する。

### 4. CSV カラム順

`harness/csv_writer.h` の `write_row` と完全一致させる:

```
library,encryption,phase,reliable,size,conns,rate,loss,
throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,
delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s
```

浮動小数点フォーマット: `loss` F3, `throughput_mbps` F3, `delivery_ratio` F4, `cpu_pct` F2 (C++ の `std::fixed << std::setprecision()` 相当)。

### 5. ウォームアップ 5 秒デフォルト

spec 要件: LiteNetLib は .NET JIT / GC のウォームアップ影響があるため、デフォルト `warmup_s = 5`(C++ アダプタのデフォルト 2 より長い)。`--warmup=N` で上書き可能。

### 6. LiteNetLib API 使用方法

```csharp
// Server
var listener = new EventBasedNetListener();
var manager = new NetManager(listener) { AutoRecycle = true };
listener.ConnectionRequestEvent += req => req.AcceptIfKey("rudpbench");
listener.NetworkReceiveEvent += (peer, reader, _, _) => { /* echo */ };
manager.Start(port);
// ループ内: manager.PollEvents()

// Client
var peer = manager.Connect(host, port, "rudpbench");
// ループ内: peer.Send(data, DeliveryMethod.ReliableOrdered | Unreliable)
//          manager.PollEvents()
```

LiteNetLib 2.x では `Send()` は `void` を返す (NetSendResult なし)。

### 7. 複数コネクション (conns > 1) の制限

LiteNetLib は1つの `NetManager` から同一 endpoint へ複数回 `Connect()` を呼んだ場合、最初の peer を返す実装になっている可能性がある。Phase 1 の `conns=1000` シナリオで実際の接続数が 1 になるリスクがあり、その場合 `delivery_ratio` は測定値として有効だが `conns` パラメータとの乖離が生じる。

**回避策候補 (Phase 2 対応)**: 各 "conn" に対して別ポートでサーバを起動する、または N 個の `NetManager` インスタンスを使う。

### 8. RTT 計測方式

C++ `runner.cc` と同形式: ペイロード先頭 16 バイトに seq (8B) + timestamp_ns (8B) を埋め込み、echo 受信時に RTT を計算。タイムスタンプは `Stopwatch.GetTimestamp()` を nanoseconds に変換したもの (単調クロック)。

## テスト結果

```
dotnet build -c Release   → Build succeeded
bash tests/test_litenetlib_smoke.sh
  delivery_ratio = 1.0033
  PASS: delivery_ratio > 0
ctest --test-dir build    → test_litenetlib_smoke PASSED
```

## 未解決事項 (Unresolved)

1. **conns > 1 の動作**: LiteNetLib の同一 endpoint への複数接続挙動が未確認。Phase 1 の 1000 conn シナリオでは実際に 1 peer しか確立されない可能性がある。
2. **オフラインビルド**: NuGet が利用できない環境では `dotnet restore` が失敗する。ベンダリング (`nuget.config` でのローカルフィード) が必要。
3. **add_custom_target の再ビルド挙動**: CMake の `add_custom_target` はスタンプファイルを使わないため `cmake --build` のたびに `dotnet build` が呼ばれるが、dotnet のインクリメンタルビルドが有効なため実際のコンパイルはスキップされる。
4. **cross-platform**: `bin/Release/net8.0/litenetlib_adapter` は Linux では app host wrapper として動作する。Windows/macOS では `dotnet publish -r <rid>` が必要な場合がある。
