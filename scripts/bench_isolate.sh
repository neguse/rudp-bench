#!/usr/bin/env bash
# Reserve isolated CPU cores for rudp-bench runs without reboot.
#
# Layout (Ryzen 7 PRO 5750GE, 8 physical / 16 logical):
#   server  : phys core 7    -> CPU 7,15
#   client  : phys cores 5,6 -> CPU 5,6,13,14   (2 phys: load generator must
#             NOT be the bottleneck -- a single phys core could not emit the
#             full offered load for heavy libs at high conns. e.g. gns @1000
#             conns on 1 phys core hit attempted_ratio=0.68 -> invalid run;
#             2 phys cores -> attempted_ratio=1.0. See docs/measurements/
#             2026-05-30-netem-limit-artifact. The harness already invalidates
#             runs with attempted_ratio<0.99, so an under-provisioned client
#             yields no data rather than wrong data -- provision it generously
#             and verify attempted_ratio==1.0 for the heaviest lib / max conns.)
#   OS/games: phys cores 0-4 -> CPU 0-4,8-12
#
# Uses cgroup v2 via systemd slice properties (AllowedCPUs). The bench
# cores are drained by confining system/user/init slices to OS cores;
# NIC IRQs are steered away; performance governor + shallow C-state on
# bench cores reduce wake-latency jitter.
#
# Requires sudo. cpupower is optional; missing-package failures are
# tolerated so the rest of setup still applies.

set -euo pipefail

OS_CPUS="0-4,8-12"
SERVER_CPUS="7,15"
CLIENT_CPUS="5,6,13,14"
BENCH_CORES_LIST="5 6 7 13 14 15"
BENCH_CORES_CSV="5,6,7,13,14,15"

setup() {
    sudo systemctl set-property -- system.slice AllowedCPUs="$OS_CPUS"
    sudo systemctl set-property -- user.slice   AllowedCPUs="$OS_CPUS"
    sudo systemctl set-property -- init.scope   AllowedCPUs="$OS_CPUS"

    for f in /proc/irq/*/smp_affinity_list; do
        echo "$OS_CPUS" | sudo tee "$f" >/dev/null 2>&1 || true
    done

    for cpu in $BENCH_CORES_LIST; do
        gov="/sys/devices/system/cpu/cpu$cpu/cpufreq/scaling_governor"
        [ -e "$gov" ] || continue
        echo performance | sudo tee "$gov" >/dev/null 2>&1 || true
    done
    sudo cpupower -c "$BENCH_CORES_CSV" idle-set -D 1 >/dev/null 2>&1 || true

    echo "isolated: server=$SERVER_CPUS client=$CLIENT_CPUS, OS=$OS_CPUS"
}

teardown() {
    sudo systemctl set-property -- system.slice AllowedCPUs=
    sudo systemctl set-property -- user.slice   AllowedCPUs=
    sudo systemctl set-property -- init.scope   AllowedCPUs=

    for f in /proc/irq/*/smp_affinity_list; do
        echo "0-15" | sudo tee "$f" >/dev/null 2>&1 || true
    done

    sudo cpupower -c "$BENCH_CORES_CSV" idle-set -E >/dev/null 2>&1 || true

    echo "restored: all 16 CPUs available, default IRQ/C-state"
}

run_in_slice() {
    local slice="$1" cpus="$2"
    shift 2
    exec sudo systemd-run --slice="$slice" \
        -p AllowedCPUs="$cpus" -p CPUWeight=10000 --pty "$@"
}

usage() {
    cat <<EOF
usage: $0 <command> [args]

commands:
  setup            reserve cores $BENCH_CORES_CSV for bench (no reboot)
  teardown         restore all 16 CPUs to default
  server <cmd...>  run <cmd> in bench-server.slice on CPUs $SERVER_CPUS
  client <cmd...>  run <cmd> in bench-client.slice on CPUs $CLIENT_CPUS
  status           show current AllowedCPUs of system/user/init slices
EOF
}

status() {
    for unit in system.slice user.slice init.scope; do
        printf '%-13s ' "$unit"
        systemctl show -p AllowedCPUs --value "$unit"
    done
}

case "${1:-}" in
    setup)    setup ;;
    teardown) teardown ;;
    server)   shift; run_in_slice bench-server.slice "$SERVER_CPUS" "$@" ;;
    client)   shift; run_in_slice bench-client.slice "$CLIENT_CPUS" "$@" ;;
    status)   status ;;
    *)        usage; exit 1 ;;
esac
