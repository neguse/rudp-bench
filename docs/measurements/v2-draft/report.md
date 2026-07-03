# rudp-bench v2 レポート(draft)

- benchspec version: 1(凍結済み)
- 状態: **draft** — 空セル = 未測定(これがそのまま残作業)。draft 値は home rig・N=1
- rig: home(Ryzen 7 PRO 5750GE、netns/veth、ARK/Minecraft 同居)

**測定対象は1枚の応答曲面 R(transport, workload, conns, regime)** であり、
以下の主張1・2はその直交する断面。workload は [`profiles.md`](../../profiles.md) の
**負荷平面(lt rate {10,20,60} pps × payload {128,200,1000} B、構造固定)**で、
実在ユースケース(br_fanout / vr_room / video_room)は平面上の anchor 注記。
capacity は regime の関数、鮮度は負荷の関数なので、どの断面かを常に明示する。

**フロアと2段判定**: regime × profile ごとに transport 非依存の物理フロア
(delivery = (1−loss)^hops、staleness = path delay + interval + sample 周期)を
併記する。フロアが絶対予算を超える cell は **infeasible**(環境の宣言であって
transport の評価ではない)。transport の評価はフロア相対(delivery / floor、
staleness − floor)で行う。例: 3%×burst16 の delivery floor は 0.941 なので、
そこで 0.94 を出す transport はフロア達成 = 満点であり「break」ではない。

## 主張1: capacity(server は何接続まで張れるか)

**quality-bounded capacity(フロア相対)**: OK = validity gates +
delivery ≥ 0.95 × floor + **staleness p99 ≤ floor + 1 送信間隔**(rate に
自己スケールする archetype 非依存 gate)。「届いているが古い」を capacity に
数えず、物理フロアを transport の責任にしない。floor が絶対予算を超える cell は
infeasible と表記。conns 二分探索。break には原因ラベル必須。
anchor セルには archetype の絶対予算(br 100ms / vr 150ms / video 150ms)での
判定を**分析時に追加**する(ヒストグラムから再計算、run は増えない)。

以下の表は `orchestrator report -sweep <dir>` が sweep 出力から自動生成する
(手で編集しない)。

**capacity @ wired(負荷平面 全 9 セル + synthetic)**:

<!-- generated:capacity-wired -->
| workload | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| r10p128 | 108 (dl) | 112 (st) | ≥128 (farm) | 223 (st) | 68 (st) | 65 (st) |
| r10p200 | 95 (dl) | 111 (st) | ≥128 (farm) | 214 (st) | 63 (st) | 66 (st) |
| r10p1000 | 47 (dl) | 25 (st) | ≥64 (farm) | 95 (st) | 68 (st) | 67 (dl) |
| r20p128 ⚓br | 98 (st) | 81 (st) | ≥128 (farm) | 164 (st) | 68 (st) | 67 (dl) |
| r20p200 | 87 (st) | 60 (st) | ≥128 (farm) | 162 (st) | 68 (st) | 67 (dl) |
| r20p1000 ⚓video | 49 (st) | 12 (st) | ≥64 (farm) | 63 (st) | 72 (st) | 67 (dl) |
| r60p128 | 98 (st) | 32 (st) | 0 (st) | 96 (st) | 65 (st) | 64 (st) |
| r60p200 ⚓vr | 90 (st) | 20 (st) | 0 (st) | 96 (st) | 64 (st) | 64 (st) |
| r60p1000 | 42 (st) | 4 (st) | 0 (st) | 33 (st) | 60 (st) | 64 (st) |
| echo (synthetic) | 801 (inv) | 836 (st) | 0 (st) | 799 (st) | 60 (st) | 60 (st) |
| reliable_echo (synthetic) | ≥1024 | ≥1024 | ≥1024 | ≥1024 | 114 (md) | 336 (md) |

*凡例: `N (code)` = capacity N・break 原因(st=staleness / dl=delivery_lt / md=delivery_md / inv=validity)、`≥N` = 探索上限まで OK、`≥N (farm)` = farm 律速で打ち切り(server の break ではない)。詳細は sweep 出力の capacity.json / results.jsonl。*
<!-- /generated:capacity-wired -->

**anchor 絶対予算判定 @ wired**(profiles.md の凍結予算、capacity 点での近似):

<!-- generated:anchors-wired -->
| anchor | transport | capacity 点の staleness p99 | 予算 | 判定 |
|---|---|---|---|---|
| r20p128 ⚓br | enet | 73ms | 100ms | ✓ |
| r20p128 ⚓br | gns | 122ms | 100ms | ✗ 予算超過 |
| r20p128 ⚓br | litenetlib | 106ms | 100ms | ✗ 予算超過 |
| r20p128 ⚓br | msquic | 81ms | 100ms | ✓ |
| r20p128 ⚓br | websocket | 65ms | 100ms | ✓ |
| r20p128 ⚓br | magiconion | 73ms | 100ms | ✓ |
| r20p1000 ⚓video | enet | 90ms | 150ms | ✓ |
| r20p1000 ⚓video | gns | 102ms | 150ms | ✓ |
| r20p1000 ⚓video | litenetlib | 106ms | 150ms | ✓ |
| r20p1000 ⚓video | msquic | 102ms | 150ms | ✓ |
| r20p1000 ⚓video | websocket | 69ms | 150ms | ✓ |
| r20p1000 ⚓video | magiconion | 73ms | 150ms | ✓ |
| r60p200 ⚓vr | enet | 53ms | 150ms | ✓ |
| r60p200 ⚓vr | gns | 47ms | 150ms | ✓ |
| r60p200 ⚓vr | msquic | 51ms | 150ms | ✓ |
| r60p200 ⚓vr | websocket | 32ms | 150ms | ✓ |
| r60p200 ⚓vr | magiconion | 34ms | 150ms | ✓ |

*anchor 予算判定は探索済み capacity 点での近似(平面 gate で探索した点のみ使用)。*
<!-- /generated:anchors-wired -->

**capacity @ loss 最悪点(3%×burst16、anchor のみ)** — wired とのペアで
環境劣化への頑健性を表す:

<!-- generated:capacity-loss-worst -->
| workload | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| r20p128 ⚓br | 7 (st) | 84 (st) | 15 (st) | 143 (st) | 0 (st) | 0 (st) |
| r20p1000 ⚓video | 63 (dl) | 0 (st) | ≥64 (farm) | 0 (st) | 0 (inv) | 0 (inv) |
| r60p200 ⚓vr | 5 (st) | 21 (st) | 0 (st) | 0 (st) | 0 (inv) | 0 (st) |

*凡例: `N (code)` = capacity N・break 原因(st=staleness / dl=delivery_lt / md=delivery_md / inv=validity)、`≥N` = 探索上限まで OK、`≥N (farm)` = farm 律速で打ち切り(server の break ではない)。詳細は sweep 出力の capacity.json / results.jsonl。*
<!-- /generated:capacity-loss-worst -->

<!-- generated:anchors-loss-worst -->
| anchor | transport | capacity 点の staleness p99 | 予算 | 判定 |
|---|---|---|---|---|
| r20p128 ⚓br | enet | 262ms | 100ms | infeasible(フロア 234ms > 予算) |
| r20p128 ⚓br | gns | 163ms | 100ms | infeasible(フロア 119ms > 予算) |
| r20p128 ⚓br | litenetlib | 163ms | 100ms | infeasible(フロア 164ms > 予算) |
| r20p128 ⚓br | msquic | 147ms | 100ms | infeasible(フロア 115ms > 予算) |
| r20p1000 ⚓video | enet | 155ms | 150ms | ✗ 予算超過 |
| r20p1000 ⚓video | litenetlib | 155ms | 150ms | ✗ 予算超過 |
| r60p200 ⚓vr | enet | 147ms | 150ms | ✓ |
| r60p200 ⚓vr | gns | 98ms | 150ms | ✓ |

*anchor 予算判定は探索済み capacity 点での近似(平面 gate で探索した点のみ使用)。*
<!-- /generated:anchors-loss-worst -->

*anchor の room 主張範囲(br 〜128 / vr 〜80 / video 〜49)を超える capacity
値は stress ceiling として読む。*

## 主張2: boundary(ネットワーク条件で各 transport の鮮度特性はどう分かれるか)

loss 平面(平均 loss% × 平均 burst 長、片道 25ms 固定)での **staleness p99 (ms)**。
確定版は **vr(r60p200)/ video(r20p1000)の 2 anchor** で測る(relay 意味論に
native な 2 点)。鮮度は負荷の関数でもあるため、本主張は**負荷アンカー付き**で
測る: 無負荷極限(下表、c4)に加え、capacity@wired の ~25% / ~75% 負荷での
再測定を行う(主張1のセルが先に埋まる必要がある — E2 の実行順はこれで決まる)。
下表は draft・N=1・5s run・**50Hz latest-value echo(synthetic、anchor 移行前の
暫定 workload)**(burst 16 列はサンプル不足 — ledger #3)。

以下は `orchestrator boundary` の出力から自動生成(セル = p99 ms / フロア ms。
フロアにはバースト黒塗り項を含む)。

**vr anchor(r60p200)**:

無負荷極限(c4):

<!-- generated:boundary-r60p200-floor -->
*未測定*
<!-- /generated:boundary-r60p200-floor -->

capacity@wired の 25% 負荷:

<!-- generated:boundary-r60p200-q25 -->
*未測定*
<!-- /generated:boundary-r60p200-q25 -->

capacity@wired の 75% 負荷:

<!-- generated:boundary-r60p200-q75 -->
*未測定*
<!-- /generated:boundary-r60p200-q75 -->

**video anchor(r20p1000)**:

無負荷極限(c4):

<!-- generated:boundary-r20p1000-floor -->
*未測定*
<!-- /generated:boundary-r20p1000-floor -->

capacity@wired の 25% 負荷:

<!-- generated:boundary-r20p1000-q25 -->
*未測定*
<!-- /generated:boundary-r20p1000-q25 -->

capacity@wired の 75% 負荷:

<!-- generated:boundary-r20p1000-q75 -->
*未測定*
<!-- /generated:boundary-r20p1000-q75 -->

(旧 draft の 50Hz echo 暫定表は anchor workload 移行に伴い削除。
capacity@wired が 0 または floor 以下の transport は割合負荷を張れないため
floor 行のみに現れる)

未測定: congested regime、等高線の局所細分。

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
vr / video anchor workload、および **25%/75% 負荷アンカーでの再測定**が必要。*

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
  --describe 出力参照)。`library-default` 併記は **anchor セル(wired / loss
  最悪点ペア)のみ**、E3 から
- 実行順・ブロック設計: draft は逐次・単一 rig。DoE 準拠の実行は E3 から
