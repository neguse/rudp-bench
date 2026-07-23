# battle — 各ライブラリ最強実装の対戦表と運用

目的: tune-to-plateau 済みの各 transport 実装を wired-v3 で測り、
raw_udp 天井比の対戦表を作る。運用ルールはルート CLAUDE.md の
「バトル(wired 測定)の運用ルール」節が正。本ドキュメントは
対戦表・手順・読み方の注意・学び・TODO のハブ。

**現状(2026-07-12)**: wired-v3 の条件(profiles.md の負荷平面)は
[ADR-0003](adr/0003-battle-matrix.md) で reference matrix としては撤回された。
下表は当時の条件に対する参考実験として保持する(ADR-0000 の位置づけ)。
比較カタログの本測定は ADR-0002/0004 の reference preset を
[reference rig](reference-rig.md) で取る経路に移行し、本表の TODO
(9 以降)はその再開まで凍結。セッション手順・運用の学びは次の
バトル(または reference campaign)でもそのまま使う現役の教訓。

## 対戦表(wired-v3 — 参考実験)

天井 = [ceiling 基準線](measurements/2026-07-10-ceiling-wired-v3.md)。
**注意: 3 workload は全て broadcast fanout(負荷 O(conns²))**。v1 の
echo 番付(線形負荷 — gns 1500 > enet 600 など)とは競技が別で、
echo に強い transport(gns/litenetlib)は fanout では順位を落とす。
echo workload の追加は TODO 参照。

| transport | r20p128 | r20p1000 | r60p200 | セッション記録 |
|---|---|---|---|---|
| raw_udp(天井) | 76 | 75 | 46 | [ceiling](measurements/2026-07-10-ceiling-wired-v3.md) |
| enet | **191**(251%) | **≥75**(天井律速) | **96**(209%) | [battle-enet](measurements/2026-07-10-battle-enet.md) |
| gns | **88**(116%) | **65**(87%) | **49**(107%) | [battle-gns](measurements/2026-07-10-battle-gns.md) |
| msquic | **158**(208%) | **55**(73%) | **107**(233%) | [battle-msquic](measurements/2026-07-10-battle-msquic.md) |
| litenetlib | **≥128**(farm censored) | **72**(96%) | **≥64**(farm censored) | [battle-litenetlib](measurements/2026-07-10-battle-litenetlib.md) |
| magiconion | **≥128**(farm censored) | **97**(129%) | **81**(176%) | [battle-magiconion](measurements/2026-07-10-battle-magiconion.md) |
| websocket | **251**(330%) | **141**(188%) | **127**(276%) | [battle-websocket](measurements/2026-07-10-battle-websocket.md) |

## reference campaign 手順(2026-07-23 protocol 凍結後の本測定)

pilot 完了([2026-07-23-pilot-wan-v1](measurements/2026-07-23-pilot-wan-v1.md))
により確定した手順。対戦計画(wan 先行 2 段構え・boot まとめ・実行順)は
[ADR-0006](adr/0006-reference-campaign-v1.md)、queue 生成は
`scripts/fleet/ref-genqueue.sh`。fleet は ADR-0005 の c8g.16xlarge spot。
破断は到達率・鮮度・期限のみで判定(kernel drop は開示のみ — ledger #29)。

1. **screening**: 2 倍刻み sweep。conns 上限は auth 系 2048 / room 512
   (pilot で auth 系が 1024/512 到達のため)
2. **confirmatory queue 生成**: `scripts/fleet/confirmatory-genqueue.sh
   <screening capacity.json> <template sweep.json> <queue> <cell>` —
   境界 ±10% の窓 × seed 3 の block を作る
3. **dispatch → 判定**: `scripts/fleet/confirmatory-analyze.sh
   <campaign-summary.json>` — 停止規則(連続 3 block の全幅 ≤5% で確定、
   不足なら必要 block 数を出力、5 block 未達は INCONCLUSIVE)を機械判定
4. **block gate**: confirmatory の sweep は `measurement_mode: "reference"`
   (block 前後の raw_udp baseline + drift gate + doctor_report 必須)
5. **preset 指名**: sweep config は `"preset"`(`ref-auth-wan-s4000-v1` 等)を
   指名できる。preset が scenario/netem/warmup/duration/drain/staleness を固定
   (上書きは config エラー)し、結果の各行に `preset_hash`(凍結条件 +
   confirmatory protocol の指紋)が入る。server 2 vCPU の budget は sweep 起動時
   に検査(block 実行では rig から注入されるため load 時ではない)
6. **凍結値の機械強制**: reference mode の drift 許容幅は下記凍結値と一致
   しないと config が弾かれる。confirmatory 窓(境界 ±10%)は conns.min>1 で
   探索してよく、min 点 fail の block は `below_range` として開示され
   confirmatory-analyze が統計から除外する

drift 許容幅(2026-07-23 owner 承認・凍結、`run.ConfirmatoryV1` が正):
凍結値の「±5%」は境界値の反復ばらつき(stopping rule)に対する規則で、
baseline drift には量ごとの単位で別に定める:

- delivery ratio(0–1 の比): 前後差 ≤ **0.010**。根拠 = session 3 の
  3 ホスト反復で実測幅 0.0026(c4–c128 の全 PASS 点)
- staleness p99(ns): 前後比 ≤ **1.10 倍**。根拠 = ヒストグラムが等比
  ~1.044 倍刻みのため 2 bin 分(A/A の anchor 45 反復は 1 bin 内)

## セッション手順

1. **契約**: 開始前に「予算 N 分 / 打ち切り M 分 / アウトプット」を合意
2. **アンカー**: raw_udp r20p128 c68(0.9×天井)を 1 点 probe。
   前セッションと乖離したら停止して調査
3. **本測定**: `sudo scripts/run-sweep.sh <config>`。
   config は `sweep-wired-v3.json` の該当 transport ブロック +
   ceiling と同じ workloads/conns/netem
4. **perf**: 破断点直下で server に attach(診断専用 run — perf は SUT を
   摂動させるので capacity 主張から除外)。adapter 自前コードの % で
   plateau を判定。**診断 config は duration を 30s に延長する**(steady
   早期成立だと run が ~20s で終わり attach が間に合わない — litenetlib
   セッションで実証)。**C# は `-p` attach だと空 capture になるため
   `-a`(system-wide)で取り、SUT/farm の comm 混在を caveat として付す**
5. **記録**: `docs/measurements/<date>-battle-<lib>.md` + 本表更新 + commit

## 読み方の注意(学び)

- **raw_udp 天井は「coalesce しない transport の天井」**。ENet のような
  MTU merge を持つ transport は小 payload で天井を 2 倍以上超えうる
  (syscall 数が 1/6〜1/9 になるため)。天井比 >100% は異常ではない。
  逆に **100% 張り付きは「ハーネス律速」の証拠**で、そのセルは ≥N と読む
- **client_stall 帰属**(farm CPU idle なのに sched p99 超過)が break に
  併記されたセルは、farm 側要因の可能性をゲートが開示している状態。
  capacity は下限気味に読む
- **境界セルは ±1 点 flap する**(ledger #18 と同族)。capacity±1 は解像度の内
- **perf attach は観測効果がある**: 診断 run は capacity 判定から除外する
- **破断の形も特性**: enet/gns は staleness の緩やかな破断、msquic は
  delivery 0 への崖(QUIC datagram の CC キュー意味論)。capacity の 1 行では
  見えない差なので、記録には break 原因を必ず残す
- sweep は同一 output_dir の結果を **cache して再実行しない** — 再測は
  dir を変えるか消す

## 運用の学び(踏んだ罠)

- **rig の CPU 隔離下では素の shell(sudo 含む)からベンチを起動できない**
  (user/system slice が OS コアに閉じ込められ taskset が EINVAL)。
  必ず `scripts/run-sweep.sh` / `run-sentinel.sh`(bench.slice transient
  unit)経由。dev-notes §2 の既知の罠
- **C# は Release で測る**(quick 用 config が Debug を指していた事故 →
  全 config を Release に統一済み、commit 29b9f03)。C/C++ の build dir は
  CMakeCache の `CMAKE_BUILD_TYPE=Release` を確認してから
- **loopback の quick run は正しさ確認まで**。farm と server が CPU を
  取り合う構成では c64 級のスケール挙動は判定不能(C# 系で実証)。
  スケールの数字は必ず隔離 rig で
- **drain/service budget は「イベント数」でなく「仕事量」で bound する**:
  broadcast 1 イベントの仕事は O(conns)。固定イベント budget は高 conns で
  制御チャネルを飢えさせる(raw_udp server・enet server で同じ病気を
  踏んだ — ledger #20、commit 2f374b1 / bbe2885)。**新しい adapter を
  書くときは server 側の drain budget を最初から入れること**
- **「proc_index=0」の INVALID は server の可能性がある**(server の hello
  も proc_index=0)。invalid メッセージに role= を追加済みだが、古いログを
  読むときは注意
- **session 実行中に build 出力(orchestrator・adapter binary)を再ビルドしない**。
  identity は attempt ごとに再計算・照合されるため、途中の rebuild 以降の全
  attempt が run_identity 不一致で invalid になる(2026-07-12 の class-mapping
  session で実証 — 再ビルド時刻 01:29:59 以降に開始した attempt だけが全滅)。
  session 中の並行作業は docs・テスト(別 build dir)に限る

## TODO キュー(バトル — 未完了分は ADR-0003 撤回に伴い凍結)

1. ~~ceiling 確定~~(済 — 76/75/46)
2. ~~enet~~(済 — 191 / ≥75 / 96)
3. ~~gns~~(済 — 88 / 65 / 49)
4. ~~msquic~~(済 — 158 / 55 / 107)
5. ~~litenetlib~~(済 — ≥128 / 72 / ≥64、2 セル farm censored)
6. ~~magiconion~~(済 — ≥128 / 97 / 81、p1000 首位)
7. ~~websocket~~(済 — 251 / 141 / 127、wired-v3 3 冠)
8. ~~clocksource 修正セッション~~(**2026-07-12 供養** — home rig は 24h サーバ
   同居のため直さず smoke 専用に格下げ。tsc 級の時刻源は reference rig
   (EC2 c8g、`docs/reference-rig.md`)の受入要件へ移管。ledger #21 参照)
9. **echo workload の追加**(v1 で見えていた「接続数スケール」軸が
   現表に無い — gns/litenetlib は echo で強豪だった。fanout 表と別列で)
10. **gns Nagle A/B**(lt の NoNagle 選択が fanout で gns を不当に
    殴っていないか — gns README の宿題)
11. **loss1 regime の対戦表**(enet throttle 無効化 #11/#12 の効果検証は
   loss 下でしか見えない。wired-v3 表が揃った後に同型のセッションで)
12. r20p1000 級の「天井律速」セルの真値化: farm 増強(procs/コア)か
    天井そのものの改善後に再測。reference rig(c8g.metal-24xl、96 コア)は
    farm コアが home 比 6 倍のため、移行で構造的に解消する見込み
13. litenetlib の farm censored 2 セル(r20p128 c256 / r60p200 c128)の真値化:
    farm の pump drain スループットが構造限界(rcvbuf 4MB 化でも drops 残存)。
    farm 増強か LNL vendor 化(SocketBufferSize 定数の変更)が必要
14. 対戦表完成後: 総括レポート(v3 report への組み込み or 独立 doc)

計測器・rig の個別 TODO は [ledger](ledger.md) が正(#5 farm rcvbuf、
#6 farm 律速判定、#18 sentinel probe マージン、#21 clocksource など)。
