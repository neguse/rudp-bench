# ADR-0006: Reference campaign v1 — 対戦計画とセッション設計

- Status: **Proposed**
- Date: 2026-07-23
- 依存: ADR-0002(方法論), ADR-0004(preset・confirmatory protocol),
  ADR-0005(fleet・1h campaign protocol)
- Decision owner: project owner

## 目的

ADR-0004 preset v1 の 6 cell(authoritative-state s1000/s4000 × lan/wan、
room-relay × lan/wan)について、tune-to-plateau 済み treatment の比較カタログ
(capacity + cost + 破断特性)を reference fleet 上で取得する。成果は
ADR-0002 Recommendation Rule の入力になる最初の有効データセットである。

## 方法論(凍結済み規則の適用 — 本 ADR で新設しない)

- 判定・停止規則・drift 許容幅は凍結値(`run.ConfirmatoryV1`、
  battle.md「reference campaign 手順」)をそのまま使う
- screening は 2 倍刻み(ramp 置換は不可判定 —
  [2026-07-18-ramp-equivalence](../measurements/2026-07-18-ramp-equivalence.md))
- fleet は c8g.16xlarge spot ≤4 台、boot gate(doctor + anchor)は ADR-0005
- 1 セッション = raw_udp anchor + 1 library(ルート CLAUDE.md のバトル運用
  ルール)。乖離停止・打ち切り・pre-registration も同ルールと ADR-0002 に従う

## Decision 1: treatment set と実行順

対象は tune-to-plateau 済み + 設計ノートありの 7 treatment。実行順:

| # | treatment | 順序の根拠 |
|---|---|---|
| 1 | enet | pilot 代表。confirmatory pipeline の end-to-end 検証を兼ねる |
| 2 | gns | C 系続行。Nagle A/B の宿題(battle TODO #10)は本測定後に診断で |
| 3 | msquic | C 系。崖型破断の記録が目的の一つ |
| 4 | litenetlib | C# 系。farm censored だった 2 セルの真値化(TODO #13) |
| 5 | magiconion | C# 系。p1000 首位の再確認 |
| 6 | websocket | TCP 系比較点(sched_is_measurand) |
| 7 | kcp | conformance が dirty-tree 証跡のみ(2026-07-12)。session 冒頭の
        probe で clean 昇格してから測る |

raw_udp は候補ではなく環境診断(anchor + envbase baseline)。wan の room で
MD が構造的に不成立(ramp-equivalence 付随データ)なため、カタログ表にも
「天井」としては載せず、envbase 境界を環境開示として別掲する。

## Decision 2: セッションの形と時間契約

**1 セッション = 1 library × 全 6 cell(両 regime)。予算 2.5h、打ち切り 3h。**

- 内訳(4 hosts 並列、pilot 実測 block 12–25 分から概算):
  boot+gate ~10 分 → class-mapping probe(6 case、~10 分)→ screening
  6 cell(~50 分)→ confirmatory 6 cell × 3 seed = 18 block(~80–100 分)
- regime 分割(1 セッション = 1 regime、~75 分 × 2)としない理由: boot と
  probe の固定費を 2 回払う上、同一 bundle・同一 fleet で lan/wan を同時に
  取る方が cell 間比較の条件が揃う。伸びたら打ち切って調査する原則は
  そのまま適用する
- ADR-0005 の「1h typical」は単一比較の campaign を想定した目標であり、
  6 cell + confirmatory を 1 セッションに束ねる本計画では 2.5h を
  pre-registration の予定時間とする(hard SLA ではない)
- MORE_BLOCKS 判定の cell は同一セッション内で seed 4–5 を追加 dispatch。
  打ち切りまでに確定しない cell は INCONCLUSIVE または穴として記録し、
  延長しない

## Decision 3: セッション手順(1 library あたり)

1. **pre-registration**: `docs/measurements/<date>-ref-<lib>.md` に予算・
   打ち切り・queue 構成・bundle commit を先に書く
2. **boot gate**: ADR-0005(doctor + anchor ±10%)
3. **class-mapping probe**: 当該 treatment の 6 case を fleet 上で再実行し
   clean 証跡へ昇格(ADR-0002 Current Execution Gate の残項目)。
   FAIL/INVALID なら測定に進まず打ち切り
4. **screening**: preset 指名 sweep(`measurement_mode: "screening"`)、
   conns 上限 auth 2048 / room 512、1 cell = 1 job
5. **confirmatory**: `confirmatory-genqueue.sh` で境界 ±10% × seed 3 を生成、
   `measurement_mode: "reference"`(preset 指名 + baseline + doctor)。
   判定は `confirmatory-analyze.sh`
6. **回収・記録**: coordinator が集約 → measurement doc 更新 + カタログ表
   (下記)更新 → PR(ADR-0005 §5)

## Decision 4: 成果物の置き場

- カタログ本表は `docs/catalog.md`(新設): cell × treatment の capacity
  (median + IQR)、censored/INCONCLUSIVE、break 原因、preset_hash、
  campaign ID。battle.md の wired-v3 表は参考実験のまま残す
- cost(RQ4)と破断特性(RQ5)は measurement doc に cell ごとに記録し、
  カタログ表からリンクする
- 推薦(Recommendation Rule の適用)は全 7 セッション完了後に別 doc。
  途中経過で順位を語らない

## 予算

1 セッション ≈ 4 hosts × 3h × spot ~$1.3/h ≈ $16。7 セッション ≈ $110-120
(≈ 1.8 万円)。月 5 万円予算の内側。再測・穴埋めで 2 倍になっても収まる。

## Open Questions(owner 判断待ち)

1. セッションの実施ペース(連日 7 本か、1 本ずつ結果レビューを挟むか)。
   提案: session 1(enet)だけ単発で回して pipeline と時間見積もりを検証し、
   以降は 2 本/日
2. envbase 境界(環境開示)を wave 冒頭に独立セッションで取るか、
   session 1 に相乗りさせるか。提案: 相乗り(baseline とは別に envbase の
   screening+confirmatory を session 1 の queue へ追加、+~30 分)

## Consequences

- battle.md の凍結 TODO(#9 echo、#10 Nagle A/B、#11 loss1 表)は本 campaign
  の対象外のまま。echo workload は preset 外であり、必要なら v2 preset の
  ADR で扱う
- 全セッション完了後、ADR-0002 Current Execution Gate の残項目
  (clean conformance・reference 実測)が閉じ、推薦 doc の作成に進める
