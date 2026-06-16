#!/usr/bin/env bash
# Reserve isolated CPU cores for rudp-bench runs without reboot.
#
# Layout (Ryzen 7 PRO 5750GE, 8 physical / 16 logical):
#   server  : phys core 7    -> CPU 7,15
#   client  : phys cores 3-6 -> CPU 3,4,5,6,11,12,13,14
#             Echo profiles use 8 client processes, so the load generator gets
#             8 logical CPUs / 4 physical cores. The harness invalidates runs
#             with attempted_ratio<0.99; still provision the generator
#             generously so client_tick does not become the measurement.
#   OS/games: phys cores 0-2 -> CPU 0-2,8-10
#
# Uses cgroup v2 via systemd slice properties (AllowedCPUs). The bench
# cores are drained by confining system/user/init slices to OS cores;
# NIC IRQs are steered away; performance governor + shallow C-state on
# bench cores reduce wake-latency jitter.
#
# Requires sudo. cpupower is optional; missing-package failures are
# tolerated so the rest of setup still applies.

set -euo pipefail

OS_CPUS="0-2,8-10"
SERVER_CPUS="7,15"
CLIENT_CPUS="3,4,5,6,11,12,13,14"
BENCH_CORES_LIST="3 4 5 6 7 11 12 13 14 15"
BENCH_CORES_CSV="3,4,5,6,7,11,12,13,14,15"

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
