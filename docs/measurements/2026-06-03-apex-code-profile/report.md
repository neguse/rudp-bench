# Apex code profile: game_server c96

目的: final saturation の `game_server` で `apex_rudp` が 96 conn から崩れる理由を、コードレベルの server profile で確認する。

## Scenario

Command:

```bash
scripts/profile_code_gprofng.sh \
  --profile=game_server \
  --library=apex_rudp \
  --conns=96 \
  --out=results/code_profile_game96_gprofng_20260602T163736Z
```

Shape:

| field | value |
|---|---:|
| mode | broadcast |
| reliable rate | 1Hz |
| unreliable rate | 20Hz |
| payload | 128B |
| conns | 96 |
| duration | 20s |
| netem | 25ms delay, 5ms jitter, 1% loss, limit 100000 |
| CPU pin | server `7,15`, client `5,6,13,14` |

Note: この profile は dirty working tree 上の apex broadcast 実験差分込みで採取した。final ranking の更新ではなく、hot path 診断用。

## Result

| metric | value |
|---|---:|
| valid | ok |
| delivery_ratio | 0.4573 |
| forward_delivery_ratio | 0.9904 |
| server_echo_accept_ratio | 0.7312 |
| return_delivery_ratio | 0.6315 |
| unreliable RTT p99 | 11.635s |
| server CPU | 131.36% |
| server CPU peak | 200.45% |

## Hot Path

`gprofng` flat/calltree profile:

| symbol / path | inclusive CPU | share |
|---|---:|---:|
| `sendmmsg` | 15.148s | 99.60% |
| `ApexRudpAdapter::send_fanout_job` | 15.125s | 99.45% |
| TX worker thread `_M_run` | 15.132s | 99.50% |
| `rudp_bench::run_server` main loop | 0.077s | 0.50% |
| `ApexRudpAdapter::poll` | 0.042s | 0.28% |
| `ApexRudpAdapter::send_many` | 0.023s | 0.15% |

Hot line:

```text
adapters/apex_rudp/apex_rudp_adapter.cc:865
int n = ::sendmmsg(fd, msgs.data() + offset, ...);
```

## Interpretation

この run では `recv`, `send_many`, vector allocation, queue lock は主因ではない。CPU はほぼ TX worker の `sendmmsg` に落ちている。

`game_server` c96 は inbound が `96 conns * 21Hz = 2016 msg/s`。broadcast は各 inbound を 96 conn に fanout するため、return 側だけで約 `193,536 datagrams/s` を server が出す。apex client adapter は多数 logical conn を少数 UDP endpoint に集約するため、loopback/netem と client receive queue も同じ箇所に集中する。

そのため、今の崩れは「server main loop が遅い」より「fanout datagram を送信側で吐き切れない」に近い。小さな user-space 最適化では capacity は大きく動きにくい。

## Shard Probe

`APEX_CLIENT_SHARDS` を opt-in で試したが、`game_server` c96 は OK まで戻らなかった。

| shards | delivery | server accept | return | server CPU | status |
|---:|---:|---:|---:|---:|---|
| 4 | 0.4505 | 0.8961 | 0.5079 | 125.08% | ok row, delivery break |
| 8 | 0.4573 | 0.9049 | 0.5107 | 124.41% | ok row, delivery break |
| 16 | 0.4430 | 0.8881 | 0.5036 | 125.14% | ok row, delivery break |
| 32 | 0.4496 | 0.8966 | 0.5064 | 124.92% | ok row, delivery break |
| per conn | 0.5673 | 0.8768 | 0.6534 | 193.49% | `client_tick` |

sharding は server accept を少し上げるが、return delivery と RTT は十分に戻らない。per-conn は client drain overhead と server CPU が重く、採用できない。

## Next Fix Direction

優先度が高いのは、`sendmmsg` に落ちている server fanout の datagram volume をどう扱うかの再設計。単純な queue/allocation 最適化や client endpoint sharding だけでは c96 を OK に戻せない。

次点で、clean HEAD と候補実装の同条件 profile を取り直す。`perf` はこの環境に入っていないため、kernel stack まで見る必要が出たら `perf` を追加する。
