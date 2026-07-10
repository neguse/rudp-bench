# battle: litenetlib(wired-v3)— 2026-07-10

セッション 6。tuned 実装(UnsyncedReceiveEvent + fanout worker +
TriggerUpdate + MtuDiscovery + native sockets + PacketPoolSize、
commit a8a14ad / 29b9f03)の wired-v3 capacity。アンカー: raw_udp r20p128
c68 PASS(4 セッション連続一致)。

| workload | capacity | 天井比 | break 原因 |
|---|---|---|---|
| r20p128 | **≥128**(censored) | ≥168% | **farm_limited**: client netns RcvbufErrors(c256 で 55k drops) |
| r20p1000 | **72** | 96% | staleness + client_stall(farm CPU 24% idle) |
| r60p200 | **≥64**(censored) | ≥139% | farm_limited(c128 で 28k drops) |

## 読みどころ

- **計測器(farm)が先に音を上げる transport**。LNL client は per-conn
  NetManager の manual pump 構造(ライブラリ制約 — servers/litenetlib/
  README.md)で、fanout 受信の drain スループットが farm 限界(ledger #9 の
  本体)。2 セルは下限開示しかできない
- それでも **censored 込みで gns を全面的に上回る**(≥128 vs 88、≥64 vs 49)。
  p1000 は 72 で enet(≥75)に迫る 2 位相当
- 旧計測(v2 E2)では farm 律速でもっと低い下限しか出ていなかった —
  tuned farm(pump 1ms 化・native sockets)で計測可能域が伸びた

## セッション中の修正

- **farm 側 kernel rcvbuf 4MB 化(reflection)**: c256 で RcvbufErrors 207k。
  LNL の SocketBufferSize は const 1MB で公開ノブなし(`NetConstants.cs:48`)、
  protected `_udpSocketv4` への reflection が唯一の手段(計測器十分性であり
  SUT 不変)。drops 207k→55k に減ったが解消はせず — 残りは buffer でなく
  pump drain スループットの構造限界。farm 増強(procs/コア)か LNL vendor が
  ないと c256 は測れない(battle.md TODO)
- 1MB 時代の結果は `results-v2/battle/litenetlib.rcvbuf1m/` に退避

## perf(r20p1000 c72、破断点、診断専用 run)

**未取得**。steady 早期成立で run が短く、attach タイミングが合わずに
capture が 65KB(有意水準未満)で終わった。診断 run は duration を 30s に
延長した専用 config で取るよう手順を改善(battle.md)。litenetlib の
plateau 判定は loopback 時代の知見(logic スレッド sendto 律速、
README)で代替し、wired perf は farm censored セルの真値化(TODO 11)と
合わせて後日。

raw 結果: `results-v2/battle/litenetlib/`(git 管理外)
