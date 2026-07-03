# rudp-bench v2 レポート(draft)

- benchspec version: 1(凍結済み)
- 状態: **draft** — 空セル = 未測定(これがそのまま残作業)。draft 値は home rig・N=1
- rig: home(Ryzen 7 PRO 5750GE、netns/veth、ARK/Minecraft 同居)

## 主張1: capacity(server は何接続まで張れるか)

conns 二分探索 × {wired, loss平面最悪点}。break には原因ラベル必須。

| profile | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| echo (mixed) | — | — | — | — | — | — |
| reliable_echo | — | — | — | — | — | — |
| game_server 型 | — | — | — | — | — | — |
| media_relay 型 | — | — | — | — | — | — |

*全セル未測定(E2 で draft、E3 で確定)。*

## 主張2: boundary(どの条件まで TCP でいいか)

loss 平面(平均 loss% × 平均 burst 長、片道 25ms 固定、50Hz latest-value echo、
c4)での **staleness p99 (ms)**。draft・N=1・5s run(burst 16 列はサンプル不足 —
ledger #3)。

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

## 主張3: 「TCP でいいのはどこまでか」(暫定)

**片道 25ms・50Hz・鮮度予算 150ms の条件で、loss ~0.1% までは TCP(gRPC)は
RUDP と同等**(staleness 3者 ~74ms、理論値どおり)。**loss 1% 以上で TCP の
staleness p99 は RUDP の 2〜3倍**(131-279ms vs 74-156ms)になり、鮮度予算を
割り始める。burst 長の効果は現サンプル数では判定不能(ledger #3)。

*確定には: N≥3 ブロック反復、残り 3 transport、congested regime、
media/game 型 profile での再確認が必要。*

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
