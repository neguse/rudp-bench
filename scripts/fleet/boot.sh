#!/usr/bin/env bash
# fleet ホストの boot 時 setup + gate(ADR-0005、reference-rig.md「boot 時 gate」)。
# cloud-init から bundle 展開ツリーの root で root として実行される。
# 責務: 最小 setup → doctor PASS まで。anchor の合否判定(fleet median 比較)は
# coordinator 側で行うため、ここでは gate 出力を所定の場所に残すだけ。
#
# usage: scripts/fleet/boot.sh -rig orchestrator/rigs/aws-c8g-8xlarge.json
set -euo pipefail

RIG=""
while [ $# -gt 0 ]; do
  case "$1" in
    -rig) RIG="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[ -n "$RIG" ] || { echo "usage: $0 -rig <rig.json>" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "boot.sh requires root" >&2; exit 1; }
cd "$(dirname "$0")/../.."

GATE_DIR=/var/lib/rudp-bench/boot-gate
mkdir -p "$GATE_DIR"
# coordinator が「実行中」と「失敗」を区別できるよう、異常終了時は FAILED を残す
trap '[ -f "$GATE_DIR/READY" ] || touch "$GATE_DIR/FAILED"' EXIT

# 初回 boot のバックグラウンドジョブ(snap seeding・apt-daily 等)が測定を
# 摂動させる(3 台目実機: cloud-init 直下の calibration だけ非 PASS)。
# boot が settle するまで待つ。本スクリプトは cloud-final の子ではなく独立
# transient unit として起動される前提(user-data 参照 — cloud-final の子だと
# この wait はデッドロックする)
systemctl is-system-running --wait > /dev/null || true

# --- 最小 setup ---------------------------------------------------------
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# ベンチ運用ツール + native adapter の動的依存(gns: protobuf / msquic: numa)
apt-get install -y -qq zstd ethtool iperf3 jq libprotobuf32t64 libnuma1 libsodium23

# nofile: 現プロセスは ulimit、ssh セッションは pam_limits、bench scope は
# systemd-run の -p LimitNOFILE で与える。systemctl daemon-reexec は使わない
# (cloud-final unit が壊れて boot.sh が orphan 化し、gate 途中で刈られる —
# 2026-07-17 の 2 台目実機で実証)
printf 'root - nofile 1048576\nubuntu - nofile 1048576\n' > /etc/security/limits.d/99-bench-nofile.conf
ulimit -n 1048576

# bundle 展開ツリーは root 所有。非 root で走る計測 unit(class-mapping probe
# 等)の出力先を先に用意する(ref1 session 1 で probe が mkdir 失敗した罠)
install -d -o ubuntu -g ubuntu results-v2

# farm 凍結構成(client rcvbuf 4MB 明示 — ledger #5)の前提。Ubuntu 既定の
# rmem_max 208KB では tuned client が fail fast する(ledger #24)。
# doctor が rig 宣言(min_rmem_max/min_wmem_max)と照合する
sysctl -q -w net.core.rmem_max=8388608 net.core.wmem_max=8388608

# 動的依存の欠落を fail fast で検出(apt リストの陳腐化を manifest 側から検証)。
# .so は対象外: 実行に必要な lib は実行ファイル側の ldd に現れる。dlopen される
# オプショナル provider(.NET の libcoreclrtraceptprovider 等)を誤検出しない
missing=0
while read -r bin; do
  case "$bin" in *.so|*.so.*) continue ;; esac
  if ldd "$bin" 2>/dev/null | grep -q 'not found'; then
    echo "ERROR: $bin に未解決の動的依存:" >&2
    ldd "$bin" | grep 'not found' >&2
    missing=1
  fi
done < <(jq -r '.binaries[].path' bundle-manifest.json)
[ "$missing" = 0 ] || exit 1

# irqbalance は isolate が固定した IRQ affinity を随時再配分してしまう
# (Ubuntu 既定で active — ref1 session 1 で doctor irq_cpu_isolation が
# flap した原因)。隔離 rig では無効化が前提
systemctl disable --now irqbalance 2>/dev/null || true

# CPU 隔離(system/user/init slice の退避 + IRQ affinity。rig json から)
./build-v2/orchestrator isolate -rig "$RIG" setup

# --- gate ----------------------------------------------------------------
# doctor は FAIL 容認なし(reference-rig.md)。bundle 展開ツリーでは
# bundle-manifest.json が source 証跡になる(doctor が hash 照合する)
./build-v2/orchestrator doctor -rig "$RIG" -repo . -output "$GATE_DIR/doctor.json"
jq -e '.ok == true' "$GATE_DIR/doctor.json" > /dev/null || {
  echo "ERROR: doctor FAIL" >&2
  jq -r '.checks[] | select(.status=="FAIL") | "\(.name): \(.detail // .observed)"' "$GATE_DIR/doctor.json" >&2
  exit 1
}

# 校正(duration invariance。netem 実効値 gate は run 時に orchestrator が行う)。
# isolate setup 後は shell が os_cpus に閉じ込められるため、bench 系の実行は
# bench.slice scope で起動する(battle.md「rig の CPU 隔離下では〜」の既知の罠)。
# 証拠(run bundle)は gate dir に永続化し、失敗時に消えないようにする。
# rlimit は scope に設定できない(LimitNOFILE は service 専用)ため、本
# スクリプトの ulimit を子が継承する
BENCH_CPUS=$(jq -r .bench_cpus "$RIG")
run_calibration() { # $1 = attempt 番号
  systemd-run --scope --slice=bench.slice -p AllowedCPUs="$BENCH_CPUS" -p CPUWeight=10000 --quiet \
    env ORCHESTRATOR="$PWD/build-v2/orchestrator" \
      CALIBRATION_DIR="$GATE_DIR/duration-invariance/attempt-$1" \
    ./calibration/duration_invariance.sh > "$GATE_DIR/duration-invariance-$1.log" 2>&1
}
# 1 回だけ retry(証拠は attempt 別に残る)。flap 自体が gate dir に開示される
if ! run_calibration 1; then
  echo "WARN: calibration attempt-1 が非 PASS。attempt-2 を実行" >&2
  run_calibration 2
fi

# anchor probe: raw_udp ref-room-lan c128(A/A の全 block が踏んだ点。
# 45 run で staleness_p99 全幅 5.3% — 2026-07-18-aa-session1-8xlarge.md)。
# 合否(fleet median 比較 ±10% 暫定)は coordinator の aggregate が行う。
# ここでは評価結果の保存まで(FAIL/INVALID も証跡として残す)
sed -e "s|__SERVER_CPUS__|$(jq -r .server_cpus "$RIG")|" \
    -e "s|__CLIENT_CPUS__|$(jq -r .client_cpus "$RIG")|" \
    -e "s|__OUTPUT_DIR__|$GATE_DIR/anchor|" \
  scripts/fleet/anchor-room-lan-c128.json.tmpl > "$GATE_DIR/anchor-config.json"
systemd-run --scope --slice=bench.slice -p AllowedCPUs="$BENCH_CPUS" -p CPUWeight=10000 --quiet \
  ./build-v2/orchestrator run -config "$GATE_DIR/anchor-config.json" \
  > "$GATE_DIR/anchor.log" 2>&1 || true

touch "$GATE_DIR/READY"
echo "boot gate PASS: $GATE_DIR/READY"
