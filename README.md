# rudp-bench

Cross-library benchmark harness for reliable UDP / RUDP / QUIC implementations.
See `docs/superpowers/specs/2026-04-28-rudp-bench-design.md` for the full design.

## Status

Plan 9 complete: harness + `raw_udp` + `mini_rudp` baselines + ENet + KCP + SLikeNet + yojimbo + GNS + LiteNetLib adapters.
Subsequent plans add UDT4, msquic adapters.

| ライブラリ | 暗号 | 状態 |
|---|---|---|
| raw_udp | off | ✅ |
| mini_rudp | off | ✅ |
| enet | off | ✅ |
| kcp | off | ✅ |
| slikenet | off | ✅ |
| yojimbo | **on** (libsodium 必須) | ✅ |
| gns | **on** (GNS デフォルト) | ✅ |
| litenetlib | off (.NET 8 独立バイナリ) | ✅ |

## Dependencies

```
sudo apt-get install libsodium-dev libmbedtls-dev                   # yojimbo
sudo apt-get install protobuf-compiler libprotobuf-dev libssl-dev   # gns
sudo apt-get install dotnet-sdk-8.0                                 # litenetlib
```

## Submodule fetch

ENet / yojimbo などの third_party ライブラリは git submodule で管理しています。
clone 直後は以下を実行してください:

```
git submodule update --init --recursive
```

## Build

`cmake --build` が C++ と .NET の両系統をまとめてビルドします:

```
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

dotnet が見つからない場合 LiteNetLib ビルドはスキップされ、他の C++ アダプタのビルドは継続します。

## Run a single scenario

```
./build/harness/rudp-bench --library=raw_udp --role=server --port=29000 --duration=10 --out=/tmp/s.csv &
./build/harness/rudp-bench --library=raw_udp --role=client --host=127.0.0.1 --port=29000 \
    --reliable=u --size=64 --conns=1 --rate=100 --duration=10 --out=/tmp/c.csv
```

## Phase 1 sweep

```
scripts/run_phase1.sh --libraries=raw_udp,mini_rudp,enet,kcp,slikenet,yojimbo,gns --results=results/phase1.csv

# with tc loss injection (requires sudo)
scripts/run_phase1.sh --libraries=raw_udp,mini_rudp,enet,kcp,slikenet,yojimbo,gns --results=results/phase1.csv --loss-injection

python3 scripts/plot.py phase1-table --in results/phase1.csv --out results/phase1_table.md
```

## 既知の挙動・制限

- `delivery_ratio` は warmup 期間との境界で `> 1.0` になることがある。warmup 中の send は計測対象外だが、その echo が post-warmup 区間に到着して received としてカウントされるため。これは loopback の RTT が warmup 終了後の echo にまで影響することによる既知のアーティファクトで、長時間ランほど影響は薄まる。
- `raw_udp` は reliable モードを持たないため、`--reliable=r` シナリオは `na` 行として記録される(計測なし)。
- **size = 65536 シナリオは現状ペイロード切り詰めが起きる**: UDP の最大データグラムサイズ(65507B)を超えるため、`sendto` が `EMSGSIZE` で失敗し送信側で記録されない、または受信側の 2KB 内部バッファで切り詰められる。Phase 2 までに `mini_rudp` でアプリ層フラグメンテーションを実装するか、シナリオ最大サイズを 8KB 程度に下げる対応が必要。
- `LatencyHist::samples_` と `DeliveryTracker::received_keys_` は計測中に成長し続けるため、高 throughput × 長時間ランで RSS 計測が harness 自身のオーバーヘッドに引っ張られる。Phase 2 実装前にリザーバサンプリング等の対策を入れる予定。
- `enet` adapter は `poll()` 末尾で 1 回だけ `enet_host_flush` を呼ぶ(ENet 標準の使い方)。`raw_udp` / `mini_rudp` は `send()` 内で kernel に即時 flush するため、高 conns 時に ENet は batching 有利・per-msg latency でやや不利の方向にバイアスする。比較時は留意のこと。
- `kcp` adapter の reliable 遅延は KCP の内部タイマ粒度(デフォルト 100ms、nodelay=1 で 10ms)に依存する。loopback ではタイマ粒度が RTT の支配項になるため、ENet / raw_udp より reliable RTT が高くなる傾向がある。`ikcp_update` 呼び出し頻度を上げることで改善できるが、CPU コストと要トレードオフ(Phase 2 バックログ)。
- `kcp` adapter の unreliable は KCP を完全バイパスし raw sendto で実装するため、同モードの RTT は raw_udp と同程度になる。信頼性は持たない。
