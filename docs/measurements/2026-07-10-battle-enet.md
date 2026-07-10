# battle: enet(wired-v3)— 2026-07-10

セッション 3。tuned 実装(throttle 無効化・zero-copy forward・drain budget、
commit 2678870 系)の wired-v3 capacity。アンカー: raw_udp r20p128 c68
(0.9×天井)PASS — [天井基準線](2026-07-10-ceiling-wired-v3.md)と整合。

| workload | capacity | 天井(raw_udp) | 天井比 | break 原因 |
|---|---|---|---|---|
| r20p128 | **191** | 76 | 251% | staleness_p99 131ms > floor+1(正直な品質破断) |
| r20p1000 | **≥75** | 75 | 100%(張り付き) | staleness + **client_stall(farm CPU 10% idle)** — ハーネス律速 |
| r60p200 | **96** | 46 | 209% | staleness + client_stall 帰属併記 |

## 読み方

- **raw_udp 天井は「coalesce しない transport の天井」**。enet は merge
  (小 payload を MTU datagram に複数連結、`LiteNetPeer` ならぬ
  `enet_protocol_send_outgoing_commands` の coalesce)で datagram 数 ≈
  syscall 数を 1/6〜1/9 に圧縮するため、p128/p200 では天井を 2〜2.5 倍
  上回る。天井比 >100% は「ハーネス測定でない」ことの証明にはならない点に
  注意(逆に 100% 張り付きの r20p1000 はハーネス律速の証拠)
- r20p1000 は 1000B ≈ 1 datagram で merge が効かず、raw_udp と同じ
  per-datagram 経路 → 天井にピン留め。enet の実力は **≥75** としか言えない
- 旧実装の loss/throttle 崩壊(ledger #11: br 98→7)と c256 crash(#8)は
  この regime(loss 0.1%)では非発現。loss1 regime での検証は別セッション

## セッション中の修正

- enet server の service budget を「イベント数」から「イベント×fanout」
  スケールに変更(接続数で除算、下限 64)。固定 4096 イベントだと c208 で
  1 呼び出し 85 万送信に膨らみ、window poll が飢えて negative window margin
  INVALID(ledger #20 と同族、r20p128 が capacity 192 で censored になった)。
  修正後 retake で 191 の正直な break として確定

## perf(r60p200 c96、破断点直下、診断専用 run)

perf attach は SUT を摂動させるため、この run は capacity 主張から除外。

| % | シンボル | 帰属 |
|---|---|---|
| 15.3% | read_hpet(kernel) | **時刻取得** |
| 9.4% | __local_bh_enable_ip | kernel net |
| 5.5% | ktime_get | 時刻取得 |
| 5.0%+2.4% | veth xmit | kernel net |
| 4.8% | srso_alias_safe_ret | Zen 系 CPU 脆弱性緩和 |
| 3.8%+2.9% | hrtimer 系 | timer |
| 3.1% | _copy_from_iter | kernel send copy |
| 2.5% | sch_direct_xmit | kernel net |
| **2.4%** | **service_once(adapter 自前コード)** | adapter |

**判定: enet アダプタ自体の無駄は 2.4% で plateau に近い。** 破断点の CPU は
kernel 送信経路 + 時刻取得が支配。

**rig レベルの発見**: システム clocksource が **hpet**(available に tsc が
無い = カーネルが TSC を無効化している)。HPET 読みは 1 回 ~0.5µs 級で、
bk_now_ns はパケット毎に呼ばれるため SUT CPU の ~21%(read_hpet+ktime_get)
が時刻取得に消えている。**tsc に直せば全 transport の天井が一律に上がる**
見込みだが、全基準線が変わる + 再起動級の変更なので、直すなら
「clocksource 修正 → ceiling 再測(~10分)→ 以後のセッション続行」を
1 セッションとして切るべき(ユーザー判断待ち)。

raw 結果: `results-v2/battle/enet/`(r20p1000, r60p200)、
`results-v2/battle/enet-r20p128-retake/`(git 管理外)
