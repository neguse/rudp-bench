# rudp-bench

Cross-library benchmark harness for reliable UDP / RUDP / QUIC implementations.
See `docs/superpowers/specs/2026-04-28-rudp-bench-design.md` for the full design.

## Status

Plan 3-9 complete (all 10 adapters): harness + `raw_udp` + `mini_rudp` baselines + ENet + KCP + SLikeNet + UDT4 + yojimbo + GNS + LiteNetLib + msquic adapters.

| ライブラリ | 暗号 | 状態 |
|---|---|---|
| raw_udp | off | ✅ |
| mini_rudp | off | ✅ |
| enet | off | ✅ |
| kcp | off | ✅ |
| slikenet | off | ✅ |
| udt4 | off | ✅ |
| yojimbo | **on** (libsodium 必須) | ✅ |
| gns | **on** (GNS デフォルト) | ✅ |
| litenetlib | off (.NET 8 独立バイナリ) | ✅ |
| msquic | **on** (QUIC TLS, self-signed cert) | ✅ |

## Capability Metadata

`results/*_scenarios.csv` には各 scenario の `supports_reliability`, `min_payload_bytes`, `max_payload_bytes`, `max_connections`, `transport_mode`, `flush_policy` が出力される。unsupported axis は実送信せず、canonical result で `valid=0` と明示する。

| library | reliable | unreliable | max payload r/u | max conns | transport r/u | flush r/u |
|---|---:|---:|---:|---:|---|---|
| raw_udp | no | yes | - / 65507 | unbounded | - / udp_datagram | unsupported / immediate |
| mini_rudp | yes | yes | 65501 / 65501 | unbounded | udp_datagram_ack / udp_datagram | immediate_retransmit_poll / immediate |
| enet | yes | yes | 65536 / 65536 | 4095 | enet_packet / enet_packet | poll_flush / poll_flush |
| kcp | yes | yes | 65536 / 65502 | unbounded | kcp_arq / udp_datagram | poll_update / immediate |
| slikenet | yes | yes | 65536 / 65536 | 1 | slikenet_message / slikenet_message | library_internal / library_internal |
| udt4 | yes | no | 65536 / - | unbounded | stream / - | blocking_stream / unsupported |
| yojimbo | yes | yes | 4096 / 4096 | 64 | yojimbo_message / yojimbo_message | poll_send_packets / poll_send_packets |
| gns | yes | yes | 65536 / 65536 | unbounded | gns_message / gns_message | no_nagle / no_nagle |
| msquic | yes | yes | 65536 / 1000 | unbounded | quic_stream / quic_datagram | async_internal / async_internal |
| litenetlib | yes | yes | 1000 / 1000 | unbounded | litenetlib_message / litenetlib_message | library_internal / library_internal |

## Dependencies

```
sudo apt-get install libsodium-dev libmbedtls-dev                   # yojimbo
sudo apt-get install protobuf-compiler libprotobuf-dev libssl-dev   # gns
sudo apt-get install dotnet-sdk-8.0                                 # litenetlib (or net10)
sudo apt-get install libnuma-dev                                    # msquic
```

UDT4 は SourceForge tarball を CMake `FetchContent` で configure 時に取得するので追加 dep 不要。

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
    --reliable=u --size=64 --conns=1 --rate=100 --duration=10 --idle=spin --out=/tmp/c.csv
```

## Phase 1 sweep

```
scripts/run_phase1.sh --libraries=raw_udp,mini_rudp,enet,kcp,slikenet,udt4,yojimbo,gns,msquic,litenetlib --results=results/phase1.csv

# conservative default matrix:
#   reliable=r,u size=64,1000 conns=1,50 rate=50 msg/sec/conn loss=0 mode=echo,broadcast

# high-rate/high-conn matrix without invalid payload sizes:
scripts/run_phase1.sh --libraries=raw_udp,mini_rudp,enet,kcp,slikenet,udt4,yojimbo,gns,msquic,litenetlib \
    --sizes=64,1000 --conns=1,1000 --rates=100,100000 --losses=0,5 \
    --modes=echo,broadcast --reliabilities=r,u --results=results/phase1_stress.csv

# optional CPU pinning for same-host runs:
scripts/run_phase1.sh --libraries=raw_udp,mini_rudp --server-cpu=0 --client-cpu=1 --results=results/phase1_pinned.csv

# with tc loss injection (requires sudo)
scripts/run_phase1.sh --libraries=raw_udp,mini_rudp,enet,kcp,slikenet,udt4,yojimbo,gns,msquic,litenetlib --results=results/phase1.csv --loss-injection

python3 scripts/run_saturation.py --libraries=mini_rudp,enet,kcp,slikenet,udt4,yojimbo,gns,msquic,litenetlib \
    --reliable=r --size=64 --conns=1 --rates=100,1000,10000,100000 --summary=results/saturation.csv

python3 scripts/plot.py phase1-table --in results/phase1.csv --out results/phase1_table.md
```

`results/phase1.csv` は比較用の canonical result で、主に `delivery_ratio` と RTT p50/p95/p99、`server_cpu_pct` だけを見る。スイープ時には調査用に `results/phase1_diagnostics.csv` と `results/phase1_scenarios.csv`、role 別 raw CSV と stdout/stderr log を含む `results/phase1_raw/<run_id>/` も出力される。client tick や attempted/accepted 状態、role 別 log path などの詳細は diagnostics 側を見る。`results/phase1_scenarios.csv` は `idle_policy`、`server_cpu_pin`、`client_cpu_pin`、`pinning_policy`、capability metadata、`flush_policy` を持ち、scheduler/pinning、unsupported axis、flush/batching の前提を scenario metadata として記録する。`scripts/run_saturation.py` は 100 -> 1k -> 10k -> 100k msg/sec/conn を順に試し、`delivery_ratio` または diagnostics の `accepted_ratio` が閾値未満になるか、`server_cpu_pct` が閾値以上になったところで次の library に進む。Phase 1 runner の idle policy は既定 `spin`、saturation helper は CPU 閾値を意味ある値にするため既定 `adaptive`。`raw_udp` の saturation は `--libraries=raw_udp --reliable=u` で別に走らせる。

## 既知の挙動・制限

- `delivery_ratio` は warmup 期間との境界で `> 1.0` になることがある。warmup 中の send は計測対象外だが、その echo が post-warmup 区間に到着して received としてカウントされるため。これは loopback の RTT が warmup 終了後の echo にまで影響することによる既知のアーティファクトで、長時間ランほど影響は薄まる。
- `raw_udp` は reliable モードを持たないため、`--reliable=r` シナリオは `na` 行として記録される(計測なし)。
- oversized payload は実送信せず `valid=0, invalid_reason=unsupported_payload` として扱う。共通 Phase 1 matrix は全 adapter / reliable mode で有効な `size=64,1000` に固定し、より大きい payload は adapter ごとの `max_payload` を確認して個別に走らせる。
- canonical result の `valid=0` は、unsupported axis、process timeout/crash、client tick failure、accepted message なしを意味する。低い `delivery_ratio` 自体は有効な性能結果として扱う。
- RTT percentile は固定ビンの bounded histogram で計算するため、latency tracking memory はメッセージ数に比例しない。delivery dedup は受信 conn ごとの sliding window で行い、`delivery_dedup_policy=sliding_window_65536_per_conn` として diagnostics に記録される。window より古い重複は新規受信として数えられる。`rss_mb` は begin/end だけでなく runner loop 中も定期サンプルした最大 RSS。
- C++ adapters with queued receive paths use reusable inbox buffers. This keeps the existing `recv()` copy-out API but recycles message storage after delivery or oversize-drop, reducing per-message heap churn once the queue is warm. LiteNetLib uses pooled byte arrays for its event inbox.
- `flush_policy` は ranking metric ではなく解釈用メタデータ。`immediate` は `send()` 内で socket に渡す実装、`poll_flush` は `poll()` 末尾で明示 flush、`poll_update` / `poll_send_packets` は protocol tick でまとめて送る実装、`library_internal` / `async_internal` はライブラリ内部スケジューラに委ねる実装を表す。`no_nagle` は batching 遅延を抑える送信フラグを使う実装、`blocking_stream` は stream 送信完了まで書き込む実装。
- `enet` adapter は `poll()` 末尾で 1 回だけ `enet_host_flush` を呼ぶ(ENet 標準の使い方)。`raw_udp` / `mini_rudp` は `send()` 内で kernel に即時 flush するため、高 conns 時に ENet は batching 有利・per-msg latency でやや不利の方向にバイアスする。比較時は `scenarios.csv` の `flush_policy` も見ること。
- `kcp` adapter の reliable 遅延は KCP の内部タイマ粒度(デフォルト 100ms、nodelay=1 で 10ms)に依存する。loopback ではタイマ粒度が RTT の支配項になるため、ENet / raw_udp より reliable RTT が高くなる傾向がある。`ikcp_update` 呼び出し頻度を上げることで改善できるが、CPU コストと要トレードオフ(Phase 2 バックログ)。
- `kcp` adapter の unreliable は KCP を完全バイパスし raw sendto で実装するため、同モードの RTT は raw_udp と同程度になる。信頼性は持たない。
- `udt4` は unreliable モードを持たないため、`--reliable=u` シナリオは `na` 行として記録される(計測なし)。adapter の `supports(false)` が false を返すことで harness が自動的に na を出力する。UDT4 は SOCK_STREAM over UDP でありメッセージ境界を持たないため、adapter 内部で 4 バイト LE 長プレフィックスによるフレーミングを行っている。ソースは system apt パッケージ `libudt-dev 4.11+dfsg1` を使用(GitHub fork は環境から到達不可のため)。
- `diagnostics.csv` の `client_*` 列は client が負荷発生器として破綻していないかを見る診断値。主目的の ranking 指標ではないが、`client_tick_ok=0` のシナリオは canonical result で `valid=0, invalid_reason=client_tick` になる。
- 同一ホストで latency を比較する場合は、client の spin が server/protocol worker から CPU を奪わないよう `--server-cpu` / `--client-cpu` で別 CPU に pin することを推奨する。未指定の場合は `pinning_policy=none` として記録され、OS scheduler 任せの結果として扱う。
