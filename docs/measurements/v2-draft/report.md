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
| r10p128 | 112 (dl) | ≥64 (farm) | ≥128 (farm) | ≥128 (farm) | 128 (st) | 128 (st) |
| r10p200 | 87 (dl) | ≥64 (farm) | ≥128 (farm) | ≥128 (farm) | 128 (st) | 128 (st) |
| r10p1000 | 49 (dl) | ≥16 (farm) | ≥64 (farm) | ≥64 (farm) | 128 (st) | 64 (st) |
| r20p128 ⚓br | 115 (dl) | ≥64 (farm) | ≥128 (farm) | ≥128 (farm) | 128 (st) | 64 (st) |
| r20p200 | ≥128 (farm) | ≥32 (farm) | ≥128 (farm) | ≥128 (farm) | 128 (st) | 64 (st) |
| r20p1000 ⚓video | 51 (dl) | ≥8 (farm) | ≥64 (farm) | ≥64 (farm) | 15 (dl) | 64 (st) |
| r60p128 | ≥64 (farm) | ≥32 (farm) | 7 (st) | ≥64 (farm) | 64 (st) | 32 (st) |
| r60p200 ⚓vr | ≥64 (farm) | ≥16 (farm) | 8 (st) | ≥64 (farm) | 64 (st) | 32 (st) |
| r60p1000 | ≥32 (farm) | ≥4 (farm) | 8 (st) | ≥16 (farm) | 32 (st) | 32 (st) |
| echo (synthetic) | 703 (md) | 683 (st) | 6 (st) | ≥512 (farm) | 128 (st) | 249 (st) |
| reliable_echo (synthetic) | 765 (md) | ≥1024 | ≥1024 | ≥1024 | 335 (md) | 761 (md) |

*凡例: `N (code)` = capacity N・break 原因(st=staleness / dl=delivery_lt / md=delivery_md / inv=validity)、`≥N` = 探索上限まで OK、`≥N (farm)` = farm 律速で打ち切り(server の break ではない)。詳細は sweep 出力の capacity.json / results.jsonl。*
<!-- /generated:capacity-wired -->

**anchor 絶対予算判定 @ wired**(profiles.md の凍結予算、capacity 点での近似):

<!-- generated:anchors-wired -->
| anchor | transport | capacity 点の staleness p99 | 予算 | 判定 |
|---|---|---|---|---|
| r20p128 ⚓br | enet | 73ms | 100ms | ✓ |
| r20p128 ⚓br | gns | 102ms | 100ms | ✗ 予算超過 |
| r20p128 ⚓br | litenetlib | 106ms | 100ms | ✗ 予算超過 |
| r20p128 ⚓br | msquic | 73ms | 100ms | ✓ |
| r20p128 ⚓br | websocket | 69ms | 100ms | ✓ |
| r20p128 ⚓br | magiconion | 81ms | 100ms | ✓ |
| r20p1000 ⚓video | enet | 81ms | 150ms | ✓ |
| r20p1000 ⚓video | gns | 90ms | 150ms | ✓ |
| r20p1000 ⚓video | litenetlib | 110ms | 150ms | ✓ |
| r20p1000 ⚓video | msquic | 81ms | 150ms | ✓ |
| r20p1000 ⚓video | websocket | 61ms | 150ms | ✓ |
| r20p1000 ⚓video | magiconion | 114ms | 150ms | ✓ |
| r60p200 ⚓vr | enet | 40ms | 150ms | ✓ |
| r60p200 ⚓vr | gns | 40ms | 150ms | ✓ |
| r60p200 ⚓vr | litenetlib | 65ms | 150ms | ✓ |
| r60p200 ⚓vr | msquic | 34ms | 150ms | ✓ |
| r60p200 ⚓vr | websocket | 40ms | 150ms | ✓ |
| r60p200 ⚓vr | magiconion | 36ms | 150ms | ✓ |

*anchor 予算判定は探索済み capacity 点での近似(平面 gate で探索した点のみ使用)。*
<!-- /generated:anchors-wired -->

**capacity @ loss 最悪点(3%×burst16、anchor のみ)** — wired とのペアで
環境劣化への頑健性を表す:

<!-- generated:capacity-loss-worst -->
| workload | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| r20p128 ⚓br | 0 (st) | ≥64 (farm) | 0 (st) | ≥128 (farm) | 0 (st) | 0 (st) |
| r20p1000 ⚓video | 0 (st) | ≥8 (farm) | ≥64 (farm) | 0 (st) | 0 (st) | 0 (st) |
| r60p200 ⚓vr | 0 (st) | ≥16 (farm) | 0 (st) | ≥64 (farm) | 0 (st) | 0 (st) |

*凡例: `N (code)` = capacity N・break 原因(st=staleness / dl=delivery_lt / md=delivery_md / inv=validity)、`≥N` = 探索上限まで OK、`≥N (farm)` = farm 律速で打ち切り(server の break ではない)。詳細は sweep 出力の capacity.json / results.jsonl。*
<!-- /generated:capacity-loss-worst -->

<!-- generated:anchors-loss-worst -->
| anchor | transport | capacity 点の staleness p99 | 予算 | 判定 |
|---|---|---|---|---|
| r20p128 ⚓br | gns | 147ms | 100ms | infeasible(フロア 122ms > 予算) |
| r20p128 ⚓br | msquic | 147ms | 100ms | infeasible(フロア 115ms > 予算) |
| r20p1000 ⚓video | gns | 212ms | 150ms | infeasible(フロア 217ms > 予算) |
| r20p1000 ⚓video | litenetlib | 139ms | 150ms | ✓ |
| r60p200 ⚓vr | gns | 90ms | 150ms | ✓ |
| r60p200 ⚓vr | msquic | 81ms | 150ms | ✓ |

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
フロアにはバースト黒塗り項を含む)。**注意: 本表は CPU 隔離導入前・旧 farm 構成
(4 procs、gcServer/MinThreads なし)での測定**。UDP 系のフロア相対の読みは
頑健だが、TCP 系の絶対値と負荷アンカーの conns 基準は隔離後の capacity で
取り直す(E3)。

**vr anchor(r60p200)**:

無負荷極限(c4):

<!-- generated:boundary-r60p200-floor -->
| loss% × burst | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| 0.1 × 1 | 69/81 | 69/81 | 98/81 | 69/81 | 86/81 | 94/81 |
| 0.1 × 4 | 69/97 | 69/97 | 94/97 | 69/97 | 77/97 | 81/97 |
| 0.1 × 16 | 69/158 | 69/158 | 98/158 | 69/158 | 73/158 | 65/158 |
| 1 × 1 | 77/81 | 77/81 | 94/81 | 77/81 | 131/81 | 131/81 |
| 1 × 4 | 69/97 | 77/97 | 98/97 | 77/97 | 147/97 | 126/97 |
| 1 × 16 | 94/158 | 98/158 | 131/158 | 110/158 | 163/158 | 475/158 |
| 3 × 1 | 86/81 | 86/81 | 106/81 | 86/81 | 172/81 | 188/81 |
| 3 × 4 | 98/97 | 90/97 | 126/97 | 90/97 | 655/97 | 491/97 |
| 3 × 16 | 126/158 | 155/158 | 196/158 | 122/158 | 16777/158 | inv |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = floor(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r60p200-floor -->

capacity@wired の 25% 負荷:

<!-- generated:boundary-r60p200-q25 -->
| loss% × burst | enet | gns | msquic | websocket | magiconion |
|---|---|---|---|---|---|
| 0.1 × 1 | 69/77 | 69/80 | 69/77 | 65/77 | 65/77 |
| 0.1 × 4 | 69/79 | 69/92 | 69/79 | 69/81 | 65/81 |
| 0.1 × 16 | 69/88 | 69/139 | 69/88 | 69/94 | 81/94 |
| 1 × 1 | 69/77 | 77/80 | 69/77 | 118/77 | 118/77 |
| 1 × 4 | 69/79 | 81/92 | 69/79 | 114/81 | 118/81 |
| 1 × 16 | 77/88 | 81/139 | 69/88 | 126/94 | 131/94 |
| 3 × 1 | 81/77 | 86/80 | 77/77 | 376/77 | 557/77 |
| 3 × 4 | 81/79 | 98/92 | 77/79 | 196/81 | 172/81 |
| 3 × 16 | 86/88 | 118/139 | 81/88 | 278/94 | 425/94 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = q25(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r60p200-q25 -->

capacity@wired の 75% 負荷:

<!-- generated:boundary-r60p200-q75 -->
| loss% × burst | enet | gns | msquic | websocket | magiconion |
|---|---|---|---|---|---|
| 0.1 × 1 | 73/76 | 73/77 | 73/76 | 65/77 | 65/77 |
| 0.1 × 4 | 73/77 | 73/81 | 90/77 | 69/78 | 65/78 |
| 0.1 × 16 | 180/80 | 73/95 | inv | 65/82 | 65/82 |
| 1 × 1 | 73/76 | 73/77 | 3538/76 | inv | 2359/77 |
| 1 × 4 | 73/77 | 73/81 | 81/77 | 2621/78 | 278/78 |
| 1 × 16 | 81/80 | 81/95 | 81/80 | 1638/82 | 360/82 |
| 3 × 1 | 81/76 | 81/77 | 86/76 | inv | 6029/77 |
| 3 × 4 | 86/77 | 81/81 | 9961/77 | inv | 3014/78 |
| 3 × 16 | 81/80 | 90/95 | 81/80 | inv | 1441/82 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = q75(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r60p200-q75 -->

**video anchor(r20p1000)**:

無負荷極限(c4):

<!-- generated:boundary-r20p1000-floor -->
| loss% × burst | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| 0.1 × 1 | 102/124 | 102/124 | 122/124 | 102/124 | 102/124 | 102/124 |
| 0.1 × 4 | 102/169 | 102/169 | 122/169 | 102/169 | 94/169 | 114/169 |
| 0.1 × 16 | 102/348 | 102/348 | 122/348 | 102/348 | 212/348 | 102/348 |
| 1 × 1 | 131/124 | 131/124 | 147/124 | 131/124 | 172/124 | 155/124 |
| 1 × 4 | 131/169 | 147/169 | 155/169 | 131/169 | 180/169 | 188/169 |
| 1 × 16 | 196/348 | 131/348 | 163/348 | 9437/348 | inv | inv |
| 3 × 1 | 155/124 | 155/124 | 163/124 | 155/124 | 294/124 | 311/124 |
| 3 × 4 | 180/169 | 163/169 | 229/169 | 155/169 | 409/169 | 409/169 |
| 3 × 16 | 294/348 | 245/348 | 425/348 | 589/348 | 6553/348 | 8126/348 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = floor(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r20p1000-floor -->

capacity@wired の 25% 負荷:

<!-- generated:boundary-r20p1000-q25 -->
| loss% × burst | enet | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|
| 0.1 × 1 | 102/114 | 122/113 | 102/113 | 102/112 | 102/112 |
| 0.1 × 4 | 102/127 | 131/122 | 102/122 | 94/121 | 102/121 |
| 0.1 × 16 | 102/178 | 131/160 | 102/160 | 102/154 | 102/157 |
| 1 × 1 | 102/114 | 126/113 | 102/113 | 188/112 | 294/112 |
| 1 × 4 | 131/127 | 126/122 | 102/122 | 147/121 | 155/121 |
| 1 × 16 | 131/178 | 131/160 | 102/160 | 180/154 | 172/157 |
| 3 × 1 | 147/114 | 155/113 | 131/113 | inv | 3407/112 |
| 3 × 4 | 155/127 | 147/122 | 122/122 | inv | 1179/121 |
| 3 × 16 | 180/178 | 155/160 | 147/160 | 720/154 | 983/157 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = q25(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r20p1000-q25 -->

capacity@wired の 75% 負荷:

<!-- generated:boundary-r20p1000-q75 -->
| loss% × burst | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| 0.1 × 1 | 102/111 | 122/115 | 126/111 | 102/111 | 94/110 | 102/110 |
| 0.1 × 4 | 102/115 | 122/133 | 126/114 | 110/114 | 102/113 | 102/113 |
| 0.1 × 16 | 102/131 | 122/204 | 126/126 | 102/126 | 102/124 | 102/125 |
| 1 × 1 | 102/111 | 122/115 | 139/111 | 110/111 | inv | 4718/110 |
| 1 × 4 | 102/115 | 131/133 | 126/114 | 110/114 | inv | 2228/113 |
| 1 × 16 | 147/131 | 155/204 | 139/126 | 122/126 | inv | 1441/125 |
| 3 × 1 | 147/111 | 155/115 | 155/111 | 147/111 | inv | 7864/110 |
| 3 × 4 | 147/115 | 163/133 | 155/114 | 147/114 | inv | 5505/113 |
| 3 × 16 | 155/131 | 212/204 | 155/126 | 122/126 | inv | 5767/125 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = q75(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r20p1000-q75 -->

(旧 draft の 50Hz echo 暫定表は anchor workload 移行に伴い削除。
capacity@wired が 0 または floor 以下の transport は割合負荷を張れないため
floor 行のみに現れる)

未測定: congested regime、等高線の局所細分。

## 主張3: transport 選択の境界条件(暫定)

境界条件は **(regime, 負荷) の2軸**で条件付ける。E2 draft(N=1・home rig)、
vr(r60p200)/ video(r20p1000)anchor・片道 25ms の loss 平面から:

- **loss ~0.1% では 6 transport の鮮度は同等**(フロア相対 +0〜20ms)。TCP 系も
  成立し、選択は運用・インフラ等の別基準で決めてよい
- **境界線は loss 1% 付近を走り、burst 長で急峻になる**: 無負荷極限の TCP 系
  p99 は 1%×b1 で UDP 系の ~1.7倍(131ms vs 77ms)にとどまるが、1%×b16 で
  3〜6倍(360-475ms)、3% では1〜2桁(0.5〜16.8s)悪化する。UDP 系
  (enet/gns/litenetlib/msquic)は 3%×b16 でもフロア相対 ±30ms に張り付く
  (バースト黒塗りは transport ではなく regime の性質としてフロア側に載る)
- **負荷が乗ると境界は低 loss 側へ動く**: capacity@wired の 75% 負荷では TCP 系は
  loss 1% から秒級に崩壊(magiconion 1.4〜7.9s、websocket は停止不全で inv —
  ledger #10)。UDP 系は同条件でもフロア相対を維持する。**「鮮度予算 150ms・
  loss ≥1% が想定される経路では TCP 系 relay は成立しない」**が現時点の境界条件
- 注意: enet は packet throttle の時定数が run duration 級のため、loss 下の値は
  duration に依存する(ledger #12 — capacity スライスの崩壊値と boundary の
  健全値は duration 差で両立している)。msquic の 75% 負荷には N=1 の不安定点
  (秒級スパイク)があり反復が必要

*確定には: N≥3 ブロック反復、congested regime、msquic q75 の不安定点の反復、
等高線(1% 近傍)の局所細分が必要。*

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
