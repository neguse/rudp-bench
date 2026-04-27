# rudp-bench

Cross-library benchmark harness for reliable UDP / RUDP / QUIC implementations.
See `docs/superpowers/specs/2026-04-28-rudp-bench-design.md` for the full design.

## Status

Plan 1 complete: harness + `raw_udp` + `mini_rudp` baselines.
Subsequent plans add ENet, KCP, SLikeNet, UDT4, yojimbo, GNS, msquic, LiteNetLib adapters.

## Build

```
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run a single scenario

```
./build/harness/rudp-bench --library=raw_udp --role=server --port=29000 --duration=10 --out=/tmp/s.csv &
./build/harness/rudp-bench --library=raw_udp --role=client --host=127.0.0.1 --port=29000 \
    --reliable=u --size=64 --conns=1 --rate=100 --duration=10 --out=/tmp/c.csv
```

## Phase 1 sweep

```
scripts/run_phase1.sh --libraries=raw_udp,mini_rudp --results=results/phase1.csv

# with tc loss injection (requires sudo)
scripts/run_phase1.sh --libraries=raw_udp,mini_rudp --results=results/phase1.csv --loss-injection

python3 scripts/plot.py phase1-table --in results/phase1.csv --out results/phase1_table.md
```

## 既知の挙動

- `delivery_ratio` は warmup 期間との境界で `> 1.0` になることがある。warmup 中の send は計測対象外だが、その echo が post-warmup 区間に到着して received としてカウントされるため。これは loopback の RTT が warmup 終了後の echo にまで影響することによる既知のアーティファクトで、長時間ランほど影響は薄まる。
- `raw_udp` は reliable モードを持たないため、`--reliable=r` シナリオは `na` 行として記録される(計測なし)。
