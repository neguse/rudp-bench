# ADR-0006: Reference campaign v1 — 対戦計画とセッション設計

- Status: **Accepted**(2026-07-23 owner 承認)
- Date: 2026-07-23
- Supersede: ルート CLAUDE.md「1 セッション = raw_udp アンカー + 1 ライブラリ」
  (2026-07-10)を Decision 3 の「1 boot 内で library を直列に隔離」へ読み替え
- 依存: ADR-0002(方法論), ADR-0004(preset・confirmatory protocol),
  ADR-0005(fleet・1h campaign protocol)
- Decision owner: project owner

## 目的

tune-to-plateau 済み treatment の比較カタログ(capacity + cost + 破断特性)を
reference fleet 上で取得する。対象 cell は ADR-0004 preset v1 のうち
**wan 3 cell を先行**し(Decision 2)、lan は絞り込み後に補完する。成果は
ADR-0002 Recommendation Rule の入力になる最初の有効データセットである。

## 方法論(凍結済み規則の適用 — 本 ADR で新設しない)

- 判定・停止規則・drift 許容幅は凍結値(`run.ConfirmatoryV1`、
  battle.md「reference campaign 手順」)をそのまま使う
- screening は 2 倍刻み(ramp 置換は不可判定 —
  [2026-07-18-ramp-equivalence](../measurements/2026-07-18-ramp-equivalence.md))
- fleet は c8g.16xlarge spot ≤4 台、boot gate(doctor + anchor)は ADR-0005
- 乖離停止・打ち切り・pre-registration はルート CLAUDE.md のバトル運用
  ルールと ADR-0002 に従う(「1 セッション = 1 library」の読み替えは
  Decision 3)

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

## Decision 2: wan 先行の 2 段構え(2026-07-23 owner 指示のコスト圧縮)

- **stage 1**: 全 7 treatment × **wan 3 cell**(auth-s1000 / auth-s4000 /
  room)+ envbase(wan)。production target(GameLift、インターネット越しの
  client)の代表 regime は wan であり、推薦の必須 regime を wan と定義する
- **stage 2**: Recommendation Rule で同等群に残った上位 treatment のみ
  lan 3 cell を補完する(下限確認)。全 treatment の lan 全載せはしない
- envbase(lan)は stage 2 と同時に取る

## Decision 3: セッションの形と時間契約

**1 セッション = 1 boot。session 1 は enet 単独(pipeline 検証)、以降は
2 library / boot。**予算: session 1 = 1.5h(打ち切り 2h)、以降 = 2.5h
(打ち切り 3h)。

- boot gate(doctor + calibration + anchor)はホストの受入検査なので、
  同一 boot 内で何 library 測っても 1 回でよい。library 間の隔離は
  従来のセッション区切りと同様に **library ごとの class-mapping probe と
  cell 境界**で保ち、「1 セッション = 1 library」ルール(2026-07-10)は
  「1 boot 内で library を直列に隔離」へ読み替える(本 ADR で supersede)
- 内訳(4 hosts、pilot 実測 block 12–22 分から概算): 1 library ≈
  probe 10 分 + screening 3 cell + confirmatory 9 block ≈ 3.6 host-hour
- **tail idle の抑制**: queue は所要見込みの長い job から配る(終盤の
  ホスト遊休を減らす)。envbase は session 1 の遊休枠に相乗りさせる
- MORE_BLOCKS 判定の cell は同一セッション内で seed 4–5 を追加 dispatch。
  打ち切りまでに確定しない cell は INCONCLUSIVE または穴として記録し、
  延長しない
- ADR-0005 の「1h typical」は単一比較想定の目標。本計画は複数 library を
  束ねるため上記予算を pre-registration に書く(hard SLA ではない)

## Decision 4: セッション手順(1 library あたり)

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

## Decision 5: 成果物の置き場

- カタログ本表は `docs/catalog.md`(新設): cell × treatment の capacity
  (median + IQR)、censored/INCONCLUSIVE、break 原因、preset_hash、
  campaign ID。battle.md の wired-v3 表は参考実験のまま残す
- cost(RQ4)と破断特性(RQ5)は measurement doc に cell ごとに記録し、
  カタログ表からリンクする
- 推薦(Recommendation Rule の適用)は stage 1 完了後に同等群を出し、
  stage 2 完了後に確定する。途中経過で順位を語らない

## 予算(stage 1)

実測作業 1 library ≈ 3.6 host-hour × spot ~$1.2/h ≈ $4.5。
7 library ≈ $31 + envbase(相乗り)~$3 + boot 固定費 $2 × 4 boot ≈
**stage 1 合計 ≈ $42(≈ 6.5 千円)**。stage 2(上位 2–3 × lan)≈ +$15–20。
再測・穴埋めで 2 倍になっても月 5 万円予算の内側。

コスト規律: 固定費・遊休も含めて見積もりに載せ、1–2 割の無駄も
「誤差」として捨てない(2026-07-23 owner 指示)。campaign 記録に
見積もり vs 実績の host-hour を残し、次回の見積もりを較正する。

## Resolved Questions(2026-07-23 owner 承認)

1. boot 間の実施ペース: session 1(enet + envbase)だけ単発で回して
   pipeline と時間・コスト見積もりを検証し、以降は 1 日 1–2 boot

## Consequences

- battle.md の凍結 TODO(#9 echo、#10 Nagle A/B、#11 loss1 表)は本 campaign
  の対象外のまま。echo workload は preset 外であり、必要なら v2 preset の
  ADR で扱う
- stage 1 完了後、ADR-0002 Current Execution Gate の残項目
  (clean conformance・reference 実測)が wan について閉じ、
  同等群の提示に進める
