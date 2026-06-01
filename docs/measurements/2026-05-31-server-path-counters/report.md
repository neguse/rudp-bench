# Server-path counters: round-trip delivery を片道に分解

**測定日:** 2026-05-31、2026-06-01 追測
**目的:** ENet/GNS の差を調べるとき、従来の `delivery_ratio=client received / client accepted` だけでは、drop が client→server、server echo submit、server→client のどこで起きたか分からなかった。benchmark 側に server-path counters を追加して、round-trip の内訳を canonical result に出す。

## 実装

- payload byte 16 を bit field 化: bit0=reliable、bit1=measurement-window。既存の 0/1 reliable 判定と互換を保ったまま、warmup 外の client send だけを server 側で数える。
- client 受信側も measurement-window bit が立った echo だけを RTT/throughput/delivery に入れる。これで warmup 中 send の遅延 echo が post-warmup received として混入する旧アーティファクトを除外する。
- server raw CSV に `server_received` / `server_echo_accepted` を追加。
- server raw CSV には channel 別の `server_received_r` / `server_received_u` / `server_echo_accepted_r` / `server_echo_accepted_u` も出す。
- reducer の canonical result に以下を追加:
  - `forward_delivery_ratio = server_received / client_outbound_messages`
  - `forward_delivery_ratio_r/u = server_received_r/u / planned_channel_outbound_messages`
  - `server_echo_accept_ratio = server_echo_accepted / expected_echo_sends`
  - `server_echo_accept_ratio_r/u = server_echo_accepted_r/u / expected_channel_echo_sends`
  - `return_delivery_ratio = client_delivered / server_echo_accepted`
  - `return_delivery_ratio_r/u = client_delivered_r/u / server_echo_accepted_r/u`
- `aggregate_runs.py` の median 対象に上記 ratio を追加。
- `run_phase1_quick.sh` の native single-client path で `--ramp-up-ms` が client に渡っていなかったため修正。
- `run_phase1_quick.sh` に `--loss` を追加。外側で netem を張る quick probe でも scenario metadata に loss を残せるようにした。
- `run_phase1_quick.sh --isolate=systemd` で ENet tuning env (`ENET_NO_THROTTLE` など)を `systemd-run -E` 経由で子 process に渡すようにした。
- `scenarios.csv` に `ramp_up_ms` を追加。接続 ramp は高 conns の安定性に影響するため、結果と一緒に残す。
- `--tail-ms` を runner / reducer / scenario metadata に追加。loss 条件で reliable retransmit を待つ tail drain を明示的に変えられるようにした。
- ENet adapter に `ENET_NO_THROTTLE=1` A/B knob を追加(default off)。ON のときは CONNECT 済み peer の packet throttle を full に固定し、`flush_policy=poll_flush_no_throttle` として metadata に出す。
- `server_cpu_pct_peak` を canonical/aggregate に追加し、diagnostics に `cpu_pct_peak` / `close_ms` / client recv drain 系を通す。平均 CPU だけでは見えない瞬間飽和と harness drain の影響を見るため。
- ENet の `ENET_BATCH_POLL=1` A/B knob を追加(default off)。`enet_host_service(event)` の per-event peer scan を避ける実験用だが、現測定では悪化したため採用しない。
- ENet の `ENET_BATCH_POLL=server|client|1`、`ENET_INITIAL_RTT_MS` / `ENET_INITIAL_RTT_VAR_MS`、`ENET_PING_MS`、`ENET_TIMEOUT_*`、`ENET_UNRELIABLE_MODE=sequenced` A/B knob を追加(default off / legacy)。server-only batch、timeout、unreliable sequenced の切り分け用。
- diagnostics に `conn_peak` / `conn_disc_transport` / `conn_disc_peer` を追加し、multi-process client では `combine_clients.py` が proc 別値を合算する。server-only batch の invalid 原因が接続断かどうかを見るため。
- server の per-loop recv drain 量を `server_recv_drained_p99` / `server_recv_drained_max` として diagnostics に追加した。旧 runner 相当の上限は `RUDP_SERVER_RECV_DRAIN_LIMIT=1024` で再現できるが、既定は上限なしに変更した。
- client raw CSV に `delivered_r` / `delivered_u` / `accepted_r` / `accepted_u` を追加し、return path の loss も channel 別に見えるようにした。LiteNetLib adapter の standalone CSV schema も C++ harness と同じ列順に追従させた。

## Smoke

```
scripts/run_phase1.sh --libraries=enet,gns --rates-r=20 --rates-u=20 \
  --sizes=64 --conns=10 --losses=0 --modes=echo --duration=2 --warmup=0 \
  --idle=spin --run-id=enet_gns_path_probe \
  --results=/tmp/enet_gns_path_results.csv \
  --diagnostics=/tmp/enet_gns_path_diagnostics.csv \
  --scenarios=/tmp/enet_gns_path_scenarios.csv \
  --raw-dir=/tmp/enet_gns_path_raw
```

結果:

| lib | valid | delivery | forward | echo_accept | return | server_received | server_echo_accepted |
|---|---:|---:|---:|---:|---:|---:|---:|
| enet | 1 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 800 | 800 |
| gns | 1 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 800 | 800 |

この smoke は性能比較ではなく、同じ accepted=800 の測定窓で server-path counters が埋まり、canonical ratios が期待通り 1.0 になることの確認。

## 1000conn Probe

追加で no-netem / 1000conn / mixed 50+50 / size=64 / duration=20s / warmup=2s / client-procs=4 を単発で確認した。これは N=1 かつ netem なしなので最終順位の根拠ではなく、path counters の読み方を見るための probe。

CPU pin なし:

| lib | delivery | forward | forward_r | forward_u | echo_accept | return | server_cpu |
|---|---:|---:|---:|---:|---:|---:|---:|
| enet | 0.8338 | 0.8338 | 1.0000 | 0.6675 | 1.0000 | 1.0000 | 82.29 |
| gns | 0.9922 | 0.9922 | 1.0000 | 0.9845 | 1.0000 | 1.0000 | 183.10 |

CPU pin あり (`server=7,15;client=5,6,13,14`):

| lib | delivery | forward | forward_r | forward_u | echo_accept | return | server_cpu |
|---|---:|---:|---:|---:|---:|---:|---:|
| enet | 0.8363 | 0.8387 | 0.8990 | 0.7785 | 1.0000 | 0.9971 | 74.73 |
| gns | 0.9160 | 0.9964 | 1.0000 | 0.9928 | 0.9195 | 0.9998 | 190.11 |

読み取り:

- pin なしでは両者とも echo return は健全で、差はほぼ forward。ENet は unreliable forward が 0.6675 まで落ちる。
- pin ありでは GNS は 1物理コア server の SMT 2レーンをほぼ使い切り、server echo submit 側が 0.9195 に下がる。それでも ENet の 0.8363 より高い。
- ENet は pin ありでも forward 欠損が主因。reliable forward も 0.8990 に落ちるが、unreliable forward 0.7785 がより悪い。
- したがって、現時点の同一マシン no-netem probe では「ENet が GNS に勝つ」形は見えていない。次に見るなら ENet の unreliable 送信方針（UNSEQUENCED と packet throttle）か、netem+N>=3 の正式条件で同じ counters を取る。

同条件 no-netem を N=3 で取り直すと、ENet は 1 run だけ GNS を上回ったが中央値ではまだ届かなかった:

| lib | N valid | delivery | forward | forward_r | forward_u | echo_accept | return | server_cpu |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| enet | 3/3 | 0.9081 | 0.9085 | 0.9590 | 0.8829 | 1.0000 | 0.9993 | 78.45 |
| gns | 3/3 | 0.9166 | 0.9909 | 1.0000 | 0.9818 | 0.9222 | 0.9999 | 190.28 |

ENet run 別 delivery は 0.9081 / 0.8258 / 0.9798。stderr に `enet_inbox_dropped` は無く、client attempted/accepted は 3本とも 1.0。したがって harness inbox overflow ではなく、ENet/OS/期限内到着の forward 側の揺れ。

`ENET_BATCH_POLL=1` は no-netem 単発で ENet delivery 0.6685、client accepted_ratio 0.9578 まで悪化した。`enet_host_service(event)` の peer scan 削減という仮説は外れで、socket/service を十分に回せず client 側送出受理が落ちる。default は従来の `poll_flush` のまま。

## Netem 25/5/1 N=3

正式条件に近い形で、pin あり / netem 25ms + jitter 5ms + loss 1% / limit 100000 / N=3 中央値を取り直した。条件は mixed 50+50、size=64、conns=1000、duration=20s、warmup=2s、idle=adaptive、client-procs=4、`server=7,15;client=5,6,13,14`。

netem:

```
sudo scripts/netem.sh apply 25 5 1 100000
...
sudo scripts/netem.sh clear
```

stock ENet:

| lib | N valid | delivery | forward | forward_r | forward_u | echo_accept | return | server_cpu |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| enet | 3/3 | 0.4705 | 0.4808 | 0.5003 | 0.4613 | 1.0000 | 0.9802 | 97.71 |
| gns | 3/3 | 0.8639 | 0.8675 | 1.0000 | 0.7351 | 1.0000 | 0.9957 | 183.53 |

`ENET_NO_THROTTLE=1`:

| lib | N valid | delivery | forward | forward_r | forward_u | echo_accept | return | server_cpu |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| enet | 3/3 | 0.3897 | 0.3952 | 0.4305 | 0.3594 | 1.0000 | 0.9850 | 97.97 |
| gns | 3/3 | 0.8658 | 0.8695 | 1.0000 | 0.7389 | 1.0000 | 0.9958 | 182.35 |

読み取り:

- netem あり N=3 でも GNS が大きく上。stock で delivery 中央値は ENet 0.4705、GNS 0.8639。
- 両者とも `server_echo_accept_ratio` と `return_delivery_ratio` はほぼ 1.0。差は server echo 後ではなく client→server forward path。
- ENet は 1% loss + 1000conn で reliable / unreliable とも forward が約 0.5 まで落ちる。server CPU は約 98% で単スレ server が飽和点。
- `ENET_NO_THROTTLE=1` は改善せず、ENet delivery を 0.4705 → 0.3897 に悪化させた。packet throttle を切る方向は採用しない。knob は原因切り分け用として default off に留める。

### tail_ms=3000

上記 stock ENet は client の tail drain が既定 500ms で、ENet の reliable retransmit を切り捨てている可能性があった。`--tail-ms=3000` を追加し、同じ pin / netem 条件で取り直した。raw run は [`data/netem_tail3000_results.csv`](data/netem_tail3000_results.csv)、集計は [`data/netem_tail3000_summary.csv`](data/netem_tail3000_summary.csv)。

| lib | N valid | delivery | forward | forward_r | forward_u | echo_accept | return | server_cpu |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| enet | 3/3 | 0.5702 | 0.5742 | 0.5944 | 0.5541 | 1.0000 | 0.9930 | 93.44 |
| gns | 3/3 | 0.8683 | 0.8720 | 1.0000 | 0.7441 | 1.0000 | 0.9957 | 171.59 |

読み取り:

- tail 3000ms により ENet は stock tail 500ms の delivery 0.4705 から 0.5702 へ改善した。既定 tail 500ms は netem+loss 下の ENet reliable 評価には短すぎる。
- それでも GNS 0.8683 には届かない。GNS は reliable forward が 1.0 を維持し、差は ENet の forward path に残る。
- ENet の RTT は p50 でも約 725ms まで伸びる。tail を伸ばすと late echo を拾えるが、ユーザ体感の遅延としては重い。
- 計測後は `sudo scripts/netem.sh clear` し、`sudo scripts/netem.sh show` で `qdisc noqueue 0: root refcnt 2` を確認した。

### server recv drain unlimited

server loop は従来 1 tick で最大 1024 message だけ `recv()` していた。netem+1000conn では ENet の server-side burst が 1024 を大きく超えるため、server recv drain の既定を上限なしに変更して再計測した。raw run は [`data/netem_return_channel_tail3000_n6_results.csv`](data/netem_return_channel_tail3000_n6_results.csv)、diagnostics は [`data/netem_return_channel_tail3000_n6_diagnostics.csv`](data/netem_return_channel_tail3000_n6_diagnostics.csv)、集計は [`data/netem_return_channel_tail3000_n6_summary.csv`](data/netem_return_channel_tail3000_n6_summary.csv)。

| lib | N valid | delivery | forward | forward_r | forward_u | return | return_r | return_u | server_cpu |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| enet | 6/6 | 0.8637 | 0.8844 | 0.9909 | 0.7740 | 0.9767 | 0.9999 | 0.9468 | 91.91 |
| gns | 6/6 | 0.8565 | 0.8602 | 1.0000 | 0.7204 | 0.9958 | 1.0000 | 0.9899 | 171.32 |

読み取り:

- ENet は stock tail3000 の delivery 0.5702 から 0.8637 まで改善し、同条件 GNS 0.8565 を N=6 median で上回った。主因は forward 0.5742 -> 0.8844 で、旧 1024 drain cap が ENet を強く過小評価していた。
- tail3000 では ENet の forward は GNS を上回る(0.8844 vs 0.8602)。特に unreliable forward は ENet 0.7740、GNS 0.7204。
- ENet は return path では GNS に負ける。reliable return は両者ほぼ 1.0 だが、unreliable return は ENet 0.9468、GNS 0.9899。canonical delivery で ENet が勝つのは、forward 側の勝ちが return 側の負けを上回るため。
- ENet の server CPU は GNS の約半分(91.91 vs 171.32)。同じ delivery 帯まで来ると、CPU 効率では ENet がかなり良い。
- この結果を受け、server recv drain の既定は上限なしに変更した。旧挙動は `RUDP_SERVER_RECV_DRAIN_LIMIT=1024` で再現できる。

tail 切り捨ての可能性を潰すため、tail 5000ms も同じ条件で確認した。raw run は [`data/netem_drain_unlimited_tail5000_results.csv`](data/netem_drain_unlimited_tail5000_results.csv)、集計は [`data/netem_drain_unlimited_tail5000_summary.csv`](data/netem_drain_unlimited_tail5000_summary.csv)。

| lib | N valid | delivery | forward | forward_r | forward_u | echo_accept | return | server_cpu |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| enet | 3/3 | 0.8419 | 0.8729 | 0.9807 | 0.7564 | 1.0000 | 0.9739 | 86.82 |
| gns | 3/3 | 0.8581 | 0.8618 | 1.0000 | 0.7236 | 1.0000 | 0.9958 | 159.63 |

tail 5000ms では ENet は改善せず、tail 3000ms の方が良かった。したがって残差は単純な tail 切り捨てではなく、ENet return path と connection churn の揺れとして見る。

### ENet A/B after tail_ms=3000

追加で ENet の role 別 batch poll、初期 RTT、timeout、unreliable mode を A/B した。結果は [`data/enet_ab_results.csv`](data/enet_ab_results.csv)。

| variant | N | valid | delivery | forward | forward_r | forward_u | accepted | disconnects | note |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| stock_tail3000 | 3 | 1 | 0.5702 | 0.5742 | 0.5944 | 0.5541 | 1.0000 | n/a | baseline |
| `ENET_BATCH_POLL=server` | 1 | 0 | 0.7806 | 0.7952 | 0.9134 | 0.6508 | 0.9835 | n/a | client accepted < 0.99 |
| batch server + conn diagnostics | 1 | 0 | 0.4759 | 0.4934 | 0.4357 | 0.4480 | 0.8962 | server 430 / client 506 | disconnects visible |
| batch server + timeout 60s | 1 | 1 | 0.5324 | 0.5569 | 0.6423 | 0.4684 | 1.0000 | 0 / 0 | disconnects removed, delivery worse than stock |
| `ENET_INITIAL_RTT_MS=100` | 1 | 1 | 0.4962 | 0.4994 | 0.5128 | 0.4860 | 1.0000 | n/a | 悪化 |
| batch server + initial RTT 100 | 1 | 0 | 0.7468 | 0.7636 | 0.8583 | 0.6141 | 0.9641 | n/a | invalid |
| `ENET_UNRELIABLE_MODE=sequenced` | 3 | 1 | 0.5144 | 0.5188 | 0.5392 | 0.4985 | 1.0000 | n/a | N=3 では悪化 |
| batch server + sequenced unreliable | 1 | 0 | 0.7587 | 0.7812 | 0.8648 | 0.6428 | 0.9649 | n/a | invalid |
| server recv drain unlimited, tail3000 | 6 | 1 | 0.8637 | 0.8844 | 0.9909 | 0.7740 | 0.9978 | server 5 / client 12 | best ENet; beats GNS delivery 0.8565 |
| server recv drain unlimited, tail5000 | 3 | 1 | 0.8419 | 0.8729 | 0.9807 | 0.7564 | 0.9950 | server 20 / client 26 | longer tail did not help |

読み取り:

- server-only batch は条件によって server forward を大きく改善するが、接続断が大量に出て client 側の accepted ratio が 0.99 を下回る。connection diagnostics 追加後の再計測では server 430 / client 506 disconnects。
- `ENET_TIMEOUT_MIN_MS=60000 ENET_TIMEOUT_MAX_MS=60000` で disconnect は消えるが、delivery は 0.5324 まで下がる。切断抑制だけでは GNS に近づかない。
- 初期 RTT を短くする方向は改善せず、stock より悪化。
- unreliable を ENet の通常 sequenced unreliable に変える案は単発では良く見えたが、N=3 中央値では stock tail3000 より悪い。default は従来どおり `UNSEQUENCED` のまま。
- valid な ENet の最良中央値は server recv drain unlimited + tail3000 の 0.8637。GNS 同条件 0.8565 を canonical delivery でも上回り、forward delivery と CPU 効率ではより大きく上回る。

## 検証

- `cmake --build build -j`
- `dotnet build adapters/litenetlib -c Release`
- `python3 tests/test_reduce_result.py`
- `python3 tests/test_combine_clients.py`
- `python3 tests/test_aggregate_runs.py`
- `python3 tests/test_phase_runner.py --build-dir build`
- `bash -n scripts/run_phase1_quick.sh scripts/run_phase1.sh`
- `python3 -m py_compile scripts/reduce_result.py scripts/aggregate_runs.py scripts/combine_clients.py scripts/run_saturation.py`
- `git diff --check`
- `ctest --test-dir build --output-on-failure`（21/21 pass）
