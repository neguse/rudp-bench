# ADR-0003: バトルの測定マトリクスと実行手順

- Status: **Proposed**
- Date: 2026-07-10
- 依存: ADR-0001, ADR-0002

## Decision

### マトリクス

| 軸 | 値 |
|---|---|
| transport | enet / gns / msquic / litenetlib / magiconion / websocket(+ raw_udp 天井) |
| shape | echo(Q1)/ broadcast(Q2) |
| workload | r20p128 / r60p200 / r20p1000(小高頻度・中高頻度・大) |
| regime | **wired-50**: 片道 25ms・loss 0.1%(RTT 50ms)/ **rough-50**: 片道 25ms・loss 1% |
| conns 範囲 | broadcast: 4..512 / echo: 4..4096(v1 実績 1500-3000 を覆う) |
| deadline | 150ms 固定(ゲーム要求として regime 非依存 — 遠い回線ほど capacity が落ちるのは現実の姿) |

- 遅延 25ms は「RTT 20ms は小さすぎる」(2026-07-10 ユーザー指摘)を受けた
  改定。delay を両 regime で固定し loss だけ振ることで Q3 を純粋比較にする
- RTT 100ms 級の第 3 regime は当面やらない。必要になったら新 ADR
- loss は既存の決定的 losstrace 機構(再生計画 Phase 1-2)で注入

### 実行順(セッション単位、各セッションは ADR-0002 の運用ルールに従う)

1. 天井 4 本: raw_udp × {echo, broadcast} × {wired-50, rough-50}
2. broadcast × wired-50 × 6 transport(Q2 主表)
3. echo × wired-50 × 6 transport(Q1 主表)
4. broadcast/echo × rough-50 × 6 transport(Q3)

合計 ~28 sweep ≈ 10-12 セッション。途中打ち切っても完了分は表として有効。

### 既存結果の扱い

- 2026-07-10 の RTT20 broadcast 表(ceiling + 6 transport)は
  「wired-v3(RTT20)参考記録」に降格。docs/battle.md に注記済み
- gns Nagle A/B、farm censored セルの真値化などの派生実験は、
  該当セッション内の contract に含める(単発でやらない)

## Consequences

- sweep の workload 語彙に echo variant が必要(現状 r20p128 等は
  broadcast 展開)。regime 定義 wired-50 / rough-50 の config 追加も必要 —
  実装はセッション 9(次)の contract に含める
- 全表完成まで ~10-12 セッション。clocksource 修正(ledger #21)を挟む
  場合は天井から取り直しになるため、**挟むなら手順 1 の前**が最安
