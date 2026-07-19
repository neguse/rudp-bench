# reference rig — virt c8g fleet 受入手順

運用方針(smoke は home rig、reference は外部 fleet)はルート CLAUDE.md「rig 運用」が正。
fleet fingerprint・spot 実行・1h campaign protocol・配布/回収経路は
[ADR-0005](adr/0005-reference-fleet.md) が正。本ドキュメントは fleet の調達条件と
受入チェックリスト。

## 調達条件

- **仮想化 c8g.16xlarge に凍結**(2026-07-19、ADR-0005 Open Decisions。
  managed treatment の farm には 56 コア級の余裕が要る — ledger #26)。
  raw_udp のみの campaign は 8xlarge 可。target(GameLift の ARM
  インスタンス)と silicon(Graviton4)を揃える。GameLift ホストは
  仮想化のため virt が target 一致
- **全 run spot**、on-demand fallback なし(ADR-0005)。多様化は AZ 方向のみ。
  サイズ・AMI の混在は fingerprint class が割れるため 1 campaign 内で禁止
- リージョンは安価な US リージョン(us-west-2 等)。ベンチは veth/netns 完結で
  リージョンは測定に影響しない
- **AMI は焼かない**: 公式 Ubuntu 24.04 LTS arm64 の stock AMI を **ID pin** して
  fingerprint の構成要素として記録する。実行物は CI がビルドした bundle で配る
- **fleet ホストは credential を持たない**(GitHub token・AWS role ともなし)。
  AWS 側の永続資産(AMI・S3・IAM role)もゼロ
- **metal(c8g.metal-24xl)は perf 診断専用**。PMU が要る深掘りセッションのみ
  単発で借りる(診断 run は capacity 主張から除外 — battle.md の既存原則)

## 配布と回収(ADR-0005 §5)

- 入力: CI の arm64 job が self-contained bundle(`bundle-<commit>.tar.zst`)を
  GitHub Release asset へ push。fleet は cloud-init で無認証 curl
- 回収: coordinator(campaign を回すローカルマシン)が block 完了ごとに
  逐次 scp。campaign 結果は PR(measurement doc + compact aggregate)+
  campaign タグの Release asset(生 bundle tar)として提出

## campaign 実行手順(coordinator)

`scripts/fleet/campaign.sh` がローカルマシンで campaign 一式を回す
(ADR-0005 enabler #3。AWS 側は tag 付き ephemeral リソースのみ)。

1. `launch -n <hosts>`: bundle 実在確認 → per-campaign SSH key/SG → spot 起動
2. `dispatch -campaign <id> -queue <dir> -deadline-min <N>`: boot gate READY を
   待って gate 証跡を回収し、job(= 1 `orchestrator block`、`<dir>/<job>/block.json`)
   を空きホストへ配布。block 完了ごとに逐次 scp 回収。失敗は requeue
   (上限 2 attempt)、SSH 不達は spot 中断とみなし requeue + ホスト離脱。
   打ち切り時刻で残った job は穴として exit 3
3. `aggregate -campaign <id>`: 回収 tar を展開し、ホスト gate 結果
   (doctor / calibration)・capacity セル・穴を `campaign-summary.json` に集約
4. `cleanup -campaign <id>`: tag 検索で instance/SG/key pair を全削除

queue の書式と placeholder(`__JOB__` / `__RIG__`)は campaign.sh 冒頭コメント
が正。smoke 用 queue は `scripts/fleet/queues/smoke/`(loss 0 — 0.1% loss は
短時間 run だと期待 drop < 1 で netem loss evidence gate に正しく落とされる
ため、smoke はパイプライン検証のみを目的に loss を置かない)。

### A/A 実験(ADR-0005)の回し方

- queue は `scripts/fleet/queues/aa/`(block × 3 seed。treatment =
  raw_udp + litenetlib、ref-room-lan の screening 条件)。regime を lan に
  するのは raw_udp が loss 下で MD を満たせない(設計開示)ため —
  [2026-07-18-ramp-equivalence](measurements/2026-07-18-ramp-equivalence.md)
- サイズごとに campaign 1 回: `config.json` の instance_type / rig を
  切り替え → `launch -n 5` → `dispatch -queue scripts/fleet/queues/aa`
  → `aggregate`。時間帯を変えて 2 セッション
- 判定は `scripts/fleet/aa-analyze.sh <campaign-summary.json>...`
  (ホスト内 median → ホスト間全幅 ≤ 5% で PASS。censored/INVALID block は
  除外して開示。dispatch の動的割当により block 数はホスト間で揃わないことが
  あるため、ホストあたり有効 block 数も判定材料として出る)

## 受入チェックリスト

旧単一ホスト時代の受入 1–9 は「CI ビルド時」と「boot 時 gate(毎回)」に分割する。

### CI ビルド時(bundle 生成、コード変更ごと)

1. toolchain: Go / CMake / C・C++ toolchain / .NET 10(arm64 runner 上)
2. arm64 ビルド: README の全ビルド(Release)+ `go test ./orchestrator/...` +
   ctest(benchkit、各 native adapter)が通ること
3. 成果物は self-contained(.NET は self-contained publish、Go は static、
   C/C++ は stock Ubuntu の glibc/libstdc++ 前提)で bundle 化し、
   commit hash を名前に含めて Release asset へ push

### boot 時 gate(fleet の各ホスト、毎 campaign)

cloud-init で最小 setup(CPU 隔離 slice、IRQ affinity、nofile、数点の apt、
bundle pull)を行った後:

1. ハードウェア確認: `lscpu` でサイズどおりの CPU 数・SMT なし・Neoverse V2
2. clocksource: `current_clocksource` と available がともに `arch_sys_counter`
3. cpufreq: `/sys/devices/system/cpu/cpu*/cpufreq/` が存在しないこと
   (固定周波数 platform。rig は `expect_fixed_frequency` を宣言)
4. doctor: `orchestrator doctor -rig <サイズ別 rig 定義> -repo . -output
   <session>/doctor.json` が **PASS**(home rig と違い FAIL 容認なし。
   1–3 は doctor が検査する)
5. raw_udp anchor 1 点 probe が fleet median の許容幅内(幅は A/A で凍結)。
   外れたホストは全 block を aggregate から拒否
6. calibration: `calibration/duration_invariance.sh` と raw UDP environment
   baseline、netem 実効値 gate

boot 時 gate が全て通ったホストだけが cell を受け取る。どれかが落ちた状態の値は
mode を問わず reference へ昇格させない。

## rig 定義

サイズ別に `orchestrator/rigs/aws-c8g-<size>.json` を置く(CPU 割当は初回受入で
実機 topology を確認して確定)。旧 `aws-c8g-metal-24xl.json` は perf 診断専用
セッションの定義として残す。
