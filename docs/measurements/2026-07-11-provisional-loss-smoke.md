# 2026-07-11 Provisional Loss Pipeline Smoke

- Status: **provisional pipeline smoke only**
- Executed: 2026-07-11 07:55--08:02 UTC
- Host: `smolcenter` / Linux `7.0.14-arch1-1` / amd64
- Source: clean commit `5c52aabace5087ce706b2a18db21cd441d300ef0`
- Measurement mode / regime: `screening` / `provisional-loss-pipeline-smoke-v1`
- Local raw bundle (gitignore対象):
  `results-v2/provisional-loss-smoke-sessions/20260711T075512Z-2202012`

このsessionが確認するのは、random loss下でDoctor、前baseline、4セルのsweep、
後baseline、manifest生成までが一巡し、各runのloss evidenceとtraffic別判定が保存されることだけである。
これはclass mappingのloss conformance、凍結preset、capacityまたは性能比較、transport推薦、
既存solution採用対自作の判断根拠ではない。

## Conditions

- 対象はENetとWebSocket、scenarioは`authoritative_state`と`room_relay`の計4セル
- server/clientを別netnsとvethへ置き、clientはserverの`10.200.0.1`へ接続
- client/server各egressにdelay 25 msとsimple random loss 1%を設定
- `loss_seed`と`loss_burst_len`は未指定。再現可能なloss列やburst/correlationは主張しない
- 3 connections、client 1 process、warmup 25 s、measurement 30 s、drain 5 s、
  staleness sampling period 10 ms
- `authoritative_state`: client input LT 13 Hz / 64 B、server state LT 20 Hz / 64 B、
  双方向MD 10 Hz / 64 B
- `room_relay`: publish LT 20 Hz / 128 B、MD 10 Hz / 64 B、対称all-to-all
- LT SLO: delivery ratio >= 0.95、staleness p99 <= 300 ms
- MD SLO: eventual delivery ratio 1.0、200 ms deadline hit ratio >= 0.95
- sweep範囲は`conns.min=max=3`。capacity表示の3は全セルで`range_limited=true`であり、
  最大収容数ではない

固定条件とrunnerの位置づけは
[`provisional-loss-smoke.md`](../provisional-loss-smoke.md)に定義している。

## Doctor And Baselines

Doctorはsource stateをclean commit `5c52aab`として確認したが、全体は`FAIL`だった。
FAIL checkは次の2件である。

- clocksourceは期待した`tsc`ではなく`hpet`。利用可能一覧は`hpet, acpi_pm`
- effective IRQ affinityの`26:3, 27:4, 54:11, 55:12, 56:13, 57:14, 58:15`が
  benchmark CPUと交差

CPU layout、benchmark CPU governor、PID 1とsystem/user/init sliceの隔離、nofile、
残留netem/netns、必須tool、source stateはPASSした。DoctorがFAILのため、この値を
reference performance dataへ昇格させない。

前後のraw UDP `environment_baseline`はともに`PASS / VALID`だった。
p99はLT staleness、dropは`effective_measurement_window_inner`のqdisc counter差分である。

| phase | LT delivery | LT p99 (ms) | C->S client-egress dropped / sent_packets | S->C server-egress dropped / sent_packets |
|---|---:|---:|---:|---:|
| before | 8816 / 9000 (0.979556) | 73.728 | 100 / 8887 | 84 / 8804 |
| after | 8822 / 9000 (0.980222) | 73.728 | 92 / 8894 | 84 / 8810 |

前後baselineの統計的drift判定は、この暫定runnerのgateに含まれない。

## Four-Cell Result

4セルすべてがattempt 1で`PASS / VALID`だった。表はtraffic別の保存値で、LT p99は
staleness p99、MD deadlineは200 ms以内のdelivery数と比率である。

| solution / traffic | LT delivery | LT p99 (ms) | MD eventual delivery | MD deadline hit |
|---|---:|---:|---:|---:|
| ENet `client_input` C->S | 1161 / 1170 (0.992308) | 102.400 | 900 / 900 (1.000000) | 899 / 900 (0.998889) |
| ENet `server_state` S->C | 1783 / 1800 (0.990556) | 77.824 | 900 / 900 (1.000000) | 900 / 900 (1.000000) |
| ENet `room_publish` / relay | 5314 / 5400 (0.984074) | 122.880 | 2700 / 2700 (1.000000) | 2700 / 2700 (1.000000) |
| WebSocket `client_input` C->S | 1170 / 1170 (1.000000) | 139.264 | 900 / 900 (1.000000) | 900 / 900 (1.000000) |
| WebSocket `server_state` S->C | 1800 / 1800 (1.000000) | 122.880 | 900 / 900 (1.000000) | 900 / 900 (1.000000) |
| WebSocket `room_publish` / relay | 5400 / 5400 (1.000000) | 155.648 | 2700 / 2700 (1.000000) | 2699 / 2700 (0.999630) |

全traffic seriesの`duplicates`は0で、各runのparticipant reportを合計した
`invalid_payload`も0だった。kernel UDP receive/drop deltaもclient/serverとも0だった。

## Directional Loss Evidence

次は各セルの`effective_measurement_window_inner`で取得したqdisc counter差分である。
configured loss 1%そのものではなく、その測定windowで実際に保存された方向別packet数を示す。

| solution / scenario | C->S client-egress dropped / sent_packets | S->C server-egress dropped / sent_packets |
|---|---:|---:|
| ENet `authoritative_state` | 24 / 2882 | 40 / 3589 |
| ENet `room_relay` | 23 / 2720 | 23 / 2725 |
| WebSocket `authoritative_state` | 26 / 2946 | 25 / 2373 |
| WebSocket `room_relay` | 35 / 3315 | 23 / 2935 |

## Integrity And Interpretation

`bundle-manifest.json`はsession内の52 regular fileを列挙する。そのmanifest自体のSHA-256は
`49756abd67c4574537ce982fec1f1f8183e099f568af694e16595c11f32cc1e2`で、
同じ値を`session-manifest.json`にも保存した。raw bundleはgitignore対象のままとし、
この測定記録とリンク更新だけをrepositoryへ追加する。

この1回の4セルPASSはpipelineの作動確認であり、loss class mappingが実装記述どおりかを
独立に識別する試験ではない。loss seed未固定、反復なし、Doctor FAIL、2 solutionだけ、
conns=3の単一点であるため、ENetとWebSocketの優劣、一般的なloss耐性、capacity、推薦、
build-vs-buyについて結論を出さない。
