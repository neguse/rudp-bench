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
フロアにはバースト黒塗り項を含む)。CPU 隔離+凍結 farm 構成での測定
(N=1 draft — TCP 系の loss 下絶対値は run 間ばらつきが大きく、確定は
E3 の N=3 集約で行う)。

**vr anchor(r60p200)**:

無負荷極限(c4):

<!-- generated:boundary-r60p200-floor -->
| loss% × burst | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| 0.1 × 1 | 69/81 | 69/81 | 98/81 | 69/81 | 90/81 | 81/81 |
| 0.1 × 4 | 69/97 | inv | 98/97 | 69/97 | 73/97 | 77/97 |
| 0.1 × 16 | 69/158 | 69/158 | 98/158 | 69/158 | 65/158 | 69/158 |
| 1 × 1 | 77/81 | 73/81 | 98/81 | 77/81 | 126/81 | 131/81 |
| 1 × 4 | 86/97 | 81/97 | 102/97 | 73/97 | 126/97 | 126/97 |
| 1 × 16 | 147/158 | 90/158 | 102/158 | 98/158 | 139/158 | 8912/158 |
| 3 × 1 | 86/81 | 81/81 | 106/81 | 81/81 | 180/81 | 196/81 |
| 3 × 4 | 102/97 | 86/97 | 110/97 | 90/97 | 180/97 | 278/97 |
| 3 × 16 | 221/158 | 155/158 | 204/158 | 147/158 | inv | 5242/158 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = floor(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r60p200-floor -->

capacity@wired の 25% 負荷:

<!-- generated:boundary-r60p200-q25 -->
| loss% × burst | enet | msquic | websocket | magiconion |
|---|---|---|---|---|
| 0.1 × 1 | 69/77 | 69/77 | 65/77 | 63/78 |
| 0.1 × 4 | 69/81 | 69/81 | 65/81 | 73/85 |
| 0.1 × 16 | 69/94 | 69/94 | 77/94 | inv |
| 1 × 1 | 73/77 | 69/77 | 118/77 | 118/78 |
| 1 × 4 | 77/81 | 69/81 | 122/81 | 122/85 |
| 1 × 16 | 77/94 | 73/94 | 126/94 | 126/113 |
| 3 × 1 | 81/77 | 81/77 | 245/77 | 155/78 |
| 3 × 4 | 81/81 | 77/81 | 262/81 | 147/85 |
| 3 × 16 | 86/94 | 86/94 | 188/94 | 720/113 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = q25(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r60p200-q25 -->

capacity@wired の 75% 負荷:

<!-- generated:boundary-r60p200-q75 -->
| loss% × burst | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| 0.1 × 1 | 69/77 | 69/78 | 98/79 | 69/77 | 65/77 | 69/77 |
| 0.1 × 4 | 69/78 | 69/82 | 94/89 | 69/78 | 69/78 | 69/79 |
| 0.1 × 16 | 69/82 | 69/100 | 94/127 | 69/82 | 69/82 | 69/88 |
| 1 × 1 | 69/77 | 73/78 | 98/79 | 69/77 | 5242/77 | 126/77 |
| 1 × 4 | 69/78 | 69/82 | 98/89 | 69/78 | 1507/78 | 126/79 |
| 1 × 16 | 77/82 | 73/100 | 106/127 | 73/82 | 393/82 | 139/88 |
| 3 × 1 | 81/77 | 81/78 | 106/79 | 81/77 | 7602/77 | 2752/77 |
| 3 × 4 | 81/78 | 81/82 | 98/89 | 77/78 | 7602/78 | 589/79 |
| 3 × 16 | 81/82 | 86/100 | 155/127 | 77/82 | inv | 360/88 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = q75(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r60p200-q75 -->

**video anchor(r20p1000)**:

無負荷極限(c4):

<!-- generated:boundary-r20p1000-floor -->
| loss% × burst | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| 0.1 × 1 | 102/124 | 102/124 | 122/124 | 102/124 | 94/124 | 94/124 |
| 0.1 × 4 | 102/169 | 102/169 | 122/169 | 102/169 | 94/169 | 102/169 |
| 0.1 × 16 | 102/348 | 102/348 | 131/348 | 102/348 | 94/348 | 102/348 |
| 1 × 1 | 131/124 | 131/124 | 155/124 | 131/124 | 188/124 | 188/124 |
| 1 × 4 | 131/169 | 131/169 | 147/169 | 122/169 | 188/169 | 188/169 |
| 1 × 16 | 147/348 | 155/348 | 139/348 | 278/348 | 7602/348 | 1900/348 |
| 3 × 1 | 155/124 | 155/124 | 163/124 | 147/124 | 253/124 | 262/124 |
| 3 × 4 | 163/169 | 172/169 | 172/169 | 155/169 | 262/169 | 425/169 |
| 3 × 16 | 262/348 | 237/348 | 278/348 | 8912/348 | 16777/348 | 19922/348 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = floor(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r20p1000-floor -->

capacity@wired の 25% 負荷:

<!-- generated:boundary-r20p1000-q25 -->
| loss% × burst | enet | litenetlib | msquic | magiconion |
|---|---|---|---|---|
| 0.1 × 1 | 102/113 | 122/113 | 102/113 | 102/113 |
| 0.1 × 4 | 102/125 | 122/122 | 102/122 | 94/122 |
| 0.1 × 16 | 102/173 | 122/160 | 102/160 | 102/160 |
| 1 × 1 | 102/113 | 114/113 | 102/113 | 147/113 |
| 1 × 4 | 122/125 | 122/122 | 102/122 | 163/122 |
| 1 × 16 | 147/173 | 122/160 | 102/160 | 172/160 |
| 3 × 1 | 147/113 | 155/113 | 147/113 | 2621/113 |
| 3 × 4 | 147/125 | 155/122 | 131/122 | 688/122 |
| 3 × 16 | 155/173 | 155/160 | 155/160 | 655/160 |

*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。`inv` = validity gate 不成立。負荷 = q25(conns は transport ごとに capacity@wired 比で決まる)。*
<!-- /generated:boundary-r20p1000-q25 -->

capacity@wired の 75% 負荷:

<!-- generated:boundary-r20p1000-q75 -->
| loss% × burst | enet | gns | litenetlib | msquic | websocket | magiconion |
|---|---|---|---|---|---|---|
| 0.1 × 1 | 102/111 | 110/119 | 126/111 | 110/111 | 94/114 | 98/111 |
| 0.1 × 4 | 102/115 | 110/147 | 126/114 | 102/114 | 94/128 | 102/114 |
| 0.1 × 16 | 102/130 | 110/258 | 126/126 | 102/126 | 102/185 | 102/126 |
| 1 × 1 | 110/111 | 131/119 | 131/111 | 110/111 | 172/114 | 3932/111 |
| 1 × 4 | 720/115 | 131/147 | 126/114 | 110/114 | 172/128 | 1835/114 |
| 1 × 16 | 131/130 | 180/258 | 126/126 | 110/126 | 327/185 | 1376/126 |
| 3 × 1 | 147/111 | 155/119 | 163/111 | 147/111 | 1966/114 | 7602/111 |
| 3 × 4 | 147/115 | 155/147 | 155/114 | 147/114 | 245/128 | 5767/114 |
| 3 × 16 | 155/130 | 204/258 | 155/126 | 147/126 | 1015/185 | 5767/126 |

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
