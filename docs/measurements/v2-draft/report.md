# rudp-bench v2 レポート(draft)

- benchspec version: 1(凍結済み)
- 状態: **draft** — 空セル = 未測定(これがそのまま残作業)。draft 値は home rig・N=1
- rig: home(Ryzen 7 PRO 5750GE、netns/veth、ARK/Minecraft 同居)

**測定対象は1枚の応答曲面 R(transport, profile, conns, regime)** であり、
以下の主張1・2はその直交する断面。capacity は regime の関数、鮮度は負荷の関数
なので、どの断面かを常に明示する。

**フロアと2段判定**: regime × profile ごとに transport 非依存の物理フロア
(delivery = (1−loss)^hops、staleness = path delay + interval + sample 周期)を
併記する。フロアが絶対予算を超える cell は **infeasible**(環境の宣言であって
transport の評価ではない)。transport の評価はフロア相対(delivery / floor、
staleness − floor)で行う。例: 3%×burst16 の delivery floor は 0.941 なので、
そこで 0.94 を出す transport はフロア達成 = 満点であり「break」ではない。

## 主張1: capacity(server は何接続まで張れるか)

**quality-bounded capacity(フロア相対)**: OK = validity gates +
delivery ≥ 0.95 × floor + staleness p99 が予算内(フロア超過分で評価)。
「届いているが古い」を capacity に数えず、物理フロアを transport の責任に
しない。floor が予算を超える cell は infeasible と表記。conns 二分探索。セルは **wired / loss 最悪点(3%×burst16)** の
ペアで、両者の差が環境劣化への頑健性を表す。break には原因ラベル必須。

| profile | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| echo (mixed) | — / — | — / — | — / — | — / — | — / — | — / — |
| reliable_echo | — / — | — / — | — / — | — / — | — / — | — / — |
| game_server 型 | — / — | — / — | — / — | — / — | — / — | — / — |
| media_relay 型 | — / — | — / — | — / — | — / — | — / — | — / — |

*全セル未測定(E2 で draft、E3 で確定)。*

## 主張2: boundary(ネットワーク条件で各 transport の鮮度特性はどう分かれるか)

loss 平面(平均 loss% × 平均 burst 長、片道 25ms 固定、50Hz latest-value echo)
での **staleness p99 (ms)**。鮮度は負荷の関数でもあるため、本主張は
**負荷アンカー付き**で測る: 無負荷極限(下表、c4)に加え、capacity@wired の
~25% / ~75% 負荷での再測定を行う(主張1のセルが先に埋まる必要がある —
E2 の実行順はこれで決まる)。下表は draft・N=1・5s run(burst 16 列は
サンプル不足 — ledger #3)。

| loss% × burst | enet | msquic | magiconion (TCP) |
|---|---|---|---|
| 0.1 × 1 | 74 | 74 | 111 |
| 0.1 × 4 | 74 | 74 | 70 |
| 0.1 × 16 | 123 | 74 | 74 |
| 1 × 1 | 74 | 82 | 131 |
| 1 × 4 | 90 | 90 | 279 |
| 1 × 16 | 90 | 156 | 70 |
| 3 × 1 | 82 | 90 | 197 |
| 3 × 4 | 82 | 90 | 221 |
| 3 × 16 | 148 | 111 | 254 |

同条件の loss-tolerant delivery: RUDP 系 0.94-1.00(drop として現れる)、
TCP 系は常に 1.000(遅延として現れる — 上の staleness に出る)。

未測定: gns / litenetlib / websocket の3列、congested regime、latest-value 系
profile(media/game 型)での boundary、等高線の局所細分。

## 主張3: transport 選択の境界条件(暫定)

境界条件は **(regime, 負荷) の2軸**で条件付ける。現 draft は無負荷極限のみ:

片道 25ms・50Hz・**無負荷極限(c4)** の latest-value トラフィックで:

- **loss ~0.1% まで**: 3者の staleness は同等(~74ms、理論値どおり)。
  鮮度の観点では差がなく、選択は運用・インフラ等の別基準で決めてよい
- **loss 1% 以上**: reliable-stream 系(TCP/gRPC)の staleness p99 は
  RUDP 系の 2〜3倍(131-279ms vs 74-156ms)。鮮度予算が 150ms 級なら
  この領域で RUDP 系が優位になる
- burst 長の効果は現サンプル数では判定不能(ledger #3)

*確定には: N≥3 ブロック反復、残り 3 transport、congested regime、
media/game 型 profile、および **25%/75% 負荷アンカーでの再測定**が必要。*

## 付録A: v1 published との突き合わせ(v2.0 完了条件)

v1 canonical echo c50(25ms + 5ms jitter + 1% loss)の v2 再現:

| | v1 | v2(combined 換算) | 判定 |
|---|---|---|---|
| msquic delivery | 0.9900 | 0.9901(lt 0.9802 = 理論値) | 一致 |
| msquic RTT p50 | 50.6ms | 51.2ms | 一致 |
| enet delivery | 0.9892 | 0.9803 | 差 0.9pt — jitter 分布差(uniform vs normal)で説明可能。jitter 除去で理論値 0.9801 に一致(ledger #1) |
| enet RTT p50 | 51.1ms | 51.2ms | 一致 |

## 付録B: 校正状態

| 項目 | 状態 |
|---|---|
| 会計零点(null)/ 故障注入(fault_inject)/ duration 不変性 | CI 常設 green |
| netem 実効値 gate(ping/iperf3) | 毎 netem run で自動。wired / v1compat 条件で実測一致を確認済み |
| 必達会計(TCP 系) | magiconion must-deliver が loss 下 delivery 1.000 |
| 縮小 broadcast(分母・dedup・fanout) | 3 transport で delivery 1.000 / duplicates 0 |
| proc 数不変性 / cross-rig 一致 | 未実施(E3) |

## 付録C: 開示

- 設定ポリシー: 現 draft はすべて `tuned-disclosed` 相当(各 transport の
  --describe 出力参照)。`library-default` 併記は E3 から
- 実行順・ブロック設計: draft は逐次・単一 rig。DoE 準拠の実行は E3 から
