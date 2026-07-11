# reference rig — EC2 c8g.metal-24xl 受入手順

運用方針(smoke は home rig、reference は外部 rig)はルート CLAUDE.md「rig 運用」が正。
本ドキュメントは reference rig の調達条件と、doctor PASS までの受入チェックリスト。

## 調達条件

- **c8g.metal-24xl**(Graviton4、96 vCPU / 192 GiB、SMT なし)。
  target(GameLift の ARM インスタンス)と silicon を揃える
- リージョンは安価な US リージョン(us-west-2 等)。ベンチは veth/netns 完結で
  リージョンは測定に影響しない
- campaign 中はインスタンスを保持する(host fingerprint が変わる stop/start をしない)。
  campaign 終了後に terminate
- screening は spot 可(中断された run は捨てる)。confirmatory は on-demand
- OS は arm64 の LTS distro を AMI ID で固定する(提案: Ubuntu 24.04 LTS。
  採用 AMI ID は最初の受入時に記録し、campaign 間で変えない)

## 受入チェックリスト(起動ごと)

rig 定義は `orchestrator/rigs/aws-c8g-metal-24xl.json`。CPU 割当(os=0-7 /
server=8-23 / client=24-95)は仮置きであり、**初回受入で実機 topology
(`lscpu`、NUMA、ENA queue 配置)を確認して確定させる**。

1. ハードウェア確認: `lscpu` で 96 CPU・SMT なし・Neoverse V2 を確認
2. clocksource: `current_clocksource` と available がともに `arch_sys_counter`
   であること(Graviton は ARM generic timer。x86 の tsc/hpet 問題は存在しない)
3. cpufreq: `/sys/devices/system/cpu/cpu*/cpufreq/` が存在しないこと
   (固定周波数 platform。rig は `expect_fixed_frequency` を宣言)
4. CPU 隔離: system/user/init slice の `AllowedCPUs` を `os_cpus` に閉じ込め、
   ENA ほか全 IRQ の affinity を `os_cpus` へ寄せる
5. nofile: soft limit >= 65535
6. toolchain: Go / CMake / C・C++ toolchain / .NET 10 / iproute2 / ethtool /
   ping / iperf3 / jq(README「Build」参照)
7. arm64 ビルド: README の全ビルド + `go test ./orchestrator/...` +
   ctest(benchkit、各 native adapter)が通ること
8. doctor: `build-v2/orchestrator doctor -rig orchestrator/rigs/aws-c8g-metal-24xl.json
   -repo . -output <session>/doctor.json` が **PASS**(home rig と違い FAIL 容認なし)
9. calibration: `calibration/duration_invariance.sh` と raw UDP environment
   baseline を実行し、netem 実効値 gate が通ること

1-9 がすべて通って初めて measurement を開始する。どれかが落ちた状態の値は
mode を問わず reference へ昇格させない。
