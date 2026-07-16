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
# bench.slice scope で起動する(battle.md「rig の CPU 隔離下では〜」の既知の罠)
BENCH_CPUS=$(jq -r .bench_cpus "$RIG")
# rlimit は scope に設定できない(LimitNOFILE は service 専用)。本スクリプトの
# ulimit を子が継承する
systemd-run --scope --slice=bench.slice -p AllowedCPUs="$BENCH_CPUS" -p CPUWeight=10000 --quiet \
  env ORCHESTRATOR="$PWD/build-v2/orchestrator" \
  ./calibration/duration_invariance.sh > "$GATE_DIR/duration-invariance.log" 2>&1

# TODO(A/A 設計時に確定): raw_udp anchor 1 点 probe の config を固定し、
# ここで実行して結果を $GATE_DIR に残す。合否(fleet median 比較)は coordinator。

touch "$GATE_DIR/READY"
echo "boot gate PASS: $GATE_DIR/READY"
