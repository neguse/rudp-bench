# A/A campaign — fleet fingerprint 立証(ADR-0005)

本ドキュメントは A/A 全セッションのハブ。session 1(8xlarge)の詳細に続き、
session 2(8xlarge・時間帯違い)と 16xlarge session 1 の結果、凍結提案を持つ。

## session 1 — c8g.8xlarge × 5

- campaign: `c20260718-091343`(bundle 034ab08、us-west-2、spot × 5)
- queue: `scripts/fleet/queues/aa/`(15 block、seed 1–15。treatment =
  raw_udp + litenetlib、ref-room-lan screening 条件 — regime を lan にする
  理由は [ramp-equivalence](2026-07-18-ramp-equivalence.md) 参照)
- 運用: 5 台全て boot gate PASS(doctor / calibration)。15/15 block 完走、
  失敗 0・requeue 0・穴 0。割当は各ホストちょうど 3 block。
  campaign 全体 ~35 分(打ち切り 60 分)
- 判定: `scripts/fleet/aa-analyze.sh <workdir>/campaign-summary.json`

## raw_udp / ref-room-lan: PASS(ホスト間全幅 2.8% ≤ 5%)

| host | block capacities | median |
|---|---|---|
| 100.23.77.215 | 142, 143, 146 | 143 |
| 16.146.26.164 | 143, 143, 143 | 143 |
| 184.34.86.231 | 146, 145, 148 | 146 |
| 44.242.139.44 | 146, 146, 145 | 146 |
| 44.251.79.241 | 144, 142, 142 | 142 |

全体 median 143、ホスト間全幅 (146−142)/143 = **2.8%**。ホスト内反復も
±1–3 で、境界 flap ±1 点(ADR-0004)と同オーダー。**c8g.8xlarge の
fleet fingerprint は raw_udp について成立**。

## litenetlib: 判定不能(farm censored — 測定器側の構造限界)

15 block 中 14 が c128 で `farm_limited: client netns UDP drop delta`
(RcvbufErrors 1.9k–22k)により censored。1 block のみ 209 まで通った
(gate 際の flap)。既知の LNL 構造(ledger #9/#13: SocketBufferSize 定数
1MB・公開ノブなし、pump drain の構造限界)が fleet でも支配的:

- host rmem_max 8MB 化(#24)は効かない(LNL 自身が 1MB しか要求しない)
- client_procs 8→24(1 proc/core)でも同一の c128 censored を確認
  (campaign `c20260718-095135` の単発 probe)— proc 数では解消しない

**結論: litenetlib は LNL vendor 化(SocketBufferSize 変更)なしに room 系の
A/A treatment として使えない。** managed 代表は magiconion に差し替える
(単発 probe で計測可能性を確認 — 下記)。litenetlib の vendor 化は
ledger #13 のまま将来課題。

## managed 代表の差し替え: litenetlib → magiconion

単発 probe(campaign `c20260718-095815`)で magiconion は capacity 154・
censored なし・正直な quality break(staleness_p99 344ms > 300ms)を確認。
managed 代表を magiconion に差し替えた(`queues/aa/` 更新済み。ユーザー一任
2026-07-18)。

## magiconion / ref-room-lan(session 1b、campaign `c20260718-100855`): 5.96% — 提案基準 5% を僅差超過

| host | block capacities | median |
|---|---|---|
| 100.23.83.86 | 150, 142, 147 | 147 |
| 16.148.193.4 | 155, 156, 160 | 156 |
| 35.87.227.229 | 151, 151, 151 | 151 |
| 44.243.0.175 | 150, 152, 143 | 150 |
| 44.244.8.45 | 151, 154, 152 | 152 |

全体 median 151、ホスト間全幅 (156−147)/151 = **5.96%**。運用は 15/15 完走・
穴 0。読み方:

- raw_udp が 2.8% で PASS しているため、**platform(fingerprint)自体の
  分散は小さい**。超過分の主因は managed ランタイム側のノイズ
  (ホスト内でも 142–160 と raw_udp の 3 倍幅)とみられる
- MDE 10%(ADR-0004)に対しては十分小さい。5% は「提案値」であり、
  managed treatment に対する基準の当て方(raw_udp = platform 判定、
  managed = 参考)は session 2 / 16xlarge の結果と併せて凍結時に決める

## session 2 — c8g.8xlarge × 5・時間帯違い(campaign `c20260718-155156`)

15/15 完走・穴 0。**raw_udp: spread 0.7%**(median 143–144)、
magiconion: 5.88%(median 149–158)。session 1 と同水準で再現 —
時間帯は raw_udp の platform 分散にほぼ効かず、magiconion の ~6% は
時間帯によらないランタイムノイズと確定。

併合(session 1+2、10 host-session): raw_udp 2.8% PASS / magiconion 7.3%。

## 16xlarge session 1 — c8g.16xlarge × 4(campaign `c20260718-163844`)

**N=4 は spot vCPU quota 300 の制約による逸脱**(16xlarge ×5 = 320 vCPU。
増枠 512 申請は 2026-07-16 起票の審査中)。15 block を 4/4/4/3 で完走・穴 0。
16xlarge の boot gate(64 CPU layout・doctor・calibration)は全ホスト PASS。

| treatment | host medians | spread | 判定 |
|---|---|---|---|
| raw_udp | 142, 143, 144, 144 | **1.4%** | PASS |
| magiconion | 152, 153, 154, 154 | **1.3%** | PASS |

**発見: magiconion の ~6% 分散は farm のコア余裕不足が原因**(切り分け
session `c20260718-182742`: 16xlarge で farm を 24 コアに制限すると
spread 5.96% に逆戻り — ホストスライス起因を棄却)。境界点の farm 総 CPU は
2 割弱で「暇」なため、平均 CPU による farm 十分性判定では検出できない
(ledger #26)。SUT median はサイズ間で一致(raw_udp 143、mo 151–153)し、
破断は server budget 200% の飽和で起きる(= 測定対象として健全)。

## 凍結提案(ADR-0005 Open Decisions — owner 判断待ち)

- **fleet instance size = c8g.16xlarge**。「PASS する最小サイズ」を
  両 treatment(raw_udp + managed)に要求すると 8xlarge は落ち
  16xlarge が最小。8xlarge は raw_udp 単独なら PASS(2.8%/0.7%)なので、
  raw_udp のみの campaign(ceiling 等)には使い得る
- anchor gate 幅の材料: raw_udp room-lan の block 単位分布は
  142–148(20 block、全サイズ・全時間帯)。幅の凍結値は 16xlarge
  session 2 を足してから算出
- 残: 16xlarge の時間帯違い session(quota 増枠後は ×5)、
  metal 参照点(virt バイアス記録 — 診断目的、任意)

## 残作業

1. 16xlarge session 2(時間帯違い)→ 判定確定 → ADR-0005 Open Decisions
   凍結(owner 承認)
2. anchor probe の boot.sh 組み込み + gate 幅設定(bundle 更新)
3. litenetlib の vendor 化(SocketBufferSize)は別課題(ledger #9/#13)
