# reference rig — virt c8g fleet 受入手順

運用方針(smoke は home rig、reference は外部 fleet)はルート CLAUDE.md「rig 運用」が正。
fleet fingerprint・spot 実行・1h campaign protocol は
[ADR-0005](adr/0005-reference-fleet.md) が正。本ドキュメントは fleet の調達条件と、
golden AMI / boot 時 gate の受入チェックリスト。

## 調達条件

- **仮想化 c8g の単一サイズ**(候補 c8g.8xlarge / 16xlarge — A/A 実験で
  PASS する最小サイズに凍結)。target(GameLift の ARM インスタンス)と
  silicon(Graviton4)を揃える。GameLift ホストは仮想化のため virt が target 一致
- **全 run spot**、on-demand fallback なし(ADR-0005)。多様化は AZ 方向のみ。
  サイズ・AMI の混在は fingerprint class が割れるため 1 campaign 内で禁止
- リージョンは安価な US リージョン(us-west-2 等)。ベンチは veth/netns 完結で
  リージョンは測定に影響しない
- OS は arm64 の LTS distro(Ubuntu 24.04 LTS)を **golden AMI** に焼き込み、
  AMI ID を fingerprint の構成要素として記録する
- **metal(c8g.metal-24xl)は perf 診断専用**。PMU が要る深掘りセッションのみ
  単発で借りる(診断 run は capacity 主張から除外 — battle.md の既存原則)

## 受入チェックリスト

旧単一ホスト時代の受入 1–9 を「AMI 焼き込み時に一度」と「boot 時 gate(毎回)」に
分割する。具体的な自動化は golden AMI 実装で確定し、本節を更新する。

### golden AMI 焼き込み時(一度)

1. ハードウェア確認: `lscpu` でサイズどおりの CPU 数・SMT なし・Neoverse V2
2. clocksource: `current_clocksource` と available がともに `arch_sys_counter`
3. cpufreq: `/sys/devices/system/cpu/cpu*/cpufreq/` が存在しないこと
   (固定周波数 platform。rig は `expect_fixed_frequency` を宣言)
4. toolchain: Go / CMake / C・C++ toolchain / .NET 10 / iproute2 / ethtool /
   ping / iperf3 / jq(README「Build」参照)
5. arm64 ビルド: README の全ビルド + `go test ./orchestrator/...` +
   ctest(benchkit、各 native adapter)が通り、Release 成果物を焼き込む
6. nofile: soft limit >= 65535
7. CPU 隔離の設定投入: system/user/init slice の `AllowedCPUs` を `os_cpus` に
   閉じ込め、IRQ affinity を `os_cpus` へ寄せる(値はサイズ別 rig 定義に従う)

### boot 時 gate(fleet の各ホスト、毎 campaign)

1. doctor: `build-v2/orchestrator doctor -rig <サイズ別 rig 定義> -repo . -output
   <session>/doctor.json` が **PASS**(home rig と違い FAIL 容認なし)
2. raw_udp anchor 1 点 probe が fleet median の許容幅内(幅は A/A で凍結)。
   外れたホストは全 block を aggregate から拒否
3. calibration: `calibration/duration_invariance.sh` と raw UDP environment
   baseline、netem 実効値 gate

boot 時 gate が全て通ったホストだけが cell を受け取る。どれかが落ちた状態の値は
mode を問わず reference へ昇格させない。

## rig 定義

サイズ別に `orchestrator/rigs/aws-c8g-<size>.json` を置く(CPU 割当は初回受入で
実機 topology を確認して確定)。旧 `aws-c8g-metal-24xl.json` は perf 診断専用
セッションの定義として残す。
