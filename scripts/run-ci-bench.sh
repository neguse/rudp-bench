#!/usr/bin/env bash
set -euo pipefail

# CI 用の軽量 loopback ベンチ。共有 runner 上の診断値であり、capacity 比較・
# reference・推薦には使わない(隔離なし・netem なし・N=1)。結果ディレクトリは
# Actions artifact として配布し、repo へは追加しない。
# 各 run の outcome は summary に記録するが、SUT の FAIL/INVALID では
# 終了コードを立てない(CI の合否は既存 smoke が担う)。result.json が
# 生成されない場合だけ失敗させる。

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

ORCHESTRATOR="${ORCHESTRATOR:-build-v2/orchestrator}"
OUT="${OUT:-results-v2/ci-bench}"
CONNS="${CONNS:-8}"
DURATION="${DURATION:-5s}"

rm -rf "$OUT"
mkdir -p "$OUT"

scenario_auth='{
  "name": "ci-auth", "kind": "authoritative_state",
  "client_input": {
    "traffic_id": 1,
    "loss_tolerant": {"rate_hz": 20, "payload_bytes": 64, "staleness_p99_ns": 300000000, "min_delivery_ratio": 0.9},
    "must_deliver": {"rate_hz": 5, "payload_bytes": 64, "deadline_ns": 200000000, "min_deadline_hit_ratio": 0.9, "min_eventual_delivery_ratio": 0.99}
  },
  "server_state": {
    "traffic_id": 2,
    "loss_tolerant": {"rate_hz": 20, "payload_bytes": 256, "staleness_p99_ns": 300000000, "min_delivery_ratio": 0.9},
    "must_deliver": {"rate_hz": 5, "payload_bytes": 64, "deadline_ns": 200000000, "min_deadline_hit_ratio": 0.9, "min_eventual_delivery_ratio": 0.99}
  }
}'
scenario_room='{
  "name": "ci-room", "kind": "room_relay",
  "room_publish": {
    "traffic_id": 3,
    "loss_tolerant": {"rate_hz": 20, "payload_bytes": 128, "staleness_p99_ns": 300000000, "min_delivery_ratio": 0.9},
    "must_deliver": {"rate_hz": 5, "payload_bytes": 64, "deadline_ns": 200000000, "min_deadline_hit_ratio": 0.9, "min_eventual_delivery_ratio": 0.99}
  }
}'

command_json() {
  local transport="$1" role="$2" port="$3"
  local server client
  case "$transport" in
    raw_udp)
      server="build-v2-rawudp/raw_udp_server"
      client="build-v2-rawudp/raw_udp_client"
      ;;
    enet)
      server="build-v2-enet/enet_server"
      client="build-v2-enet/enet_client"
      ;;
    kcp)
      server="build-v2-kcp/kcp_server"
      client="build-v2-kcp/kcp_client"
      ;;
    litenetlib)
      server="servers/litenetlib/LiteNetLibBench.Server/bin/Release/net10.0/LiteNetLibBench.Server"
      client="servers/litenetlib/LiteNetLibBench.Client/bin/Release/net10.0/LiteNetLibBench.Client"
      ;;
    websocket)
      server="servers/websocket/WebSocketBench.Server/bin/Release/net10.0/WebSocketBench.Server"
      client="servers/websocket/WebSocketBench.Client/bin/Release/net10.0/WebSocketBench.Client"
      ;;
    magiconion)
      server="servers/magiconion/MagicOnionBench.Server/bin/Release/net10.0/MagicOnionBench.Server"
      client="servers/magiconion/MagicOnionBench.Client/bin/Release/net10.0/MagicOnionBench.Client"
      ;;
    *)
      echo "unknown transport $transport" >&2
      return 1
      ;;
  esac
  if [[ "$role" == server ]]; then
    jq -cn --arg bin "$server" --arg port "$port" '[$bin, "--port", $port]'
  else
    jq -cn --arg bin "$client" --arg port "$port" \
      '[$bin, "--host", "127.0.0.1", "--port", $port,
        "--conns", "{conns}", "--proc-index", "{proc_index}", "--origin-base", "{origin_id_start}"]'
  fi
}

sched_is_measurand() {
  case "$1" in
    websocket | magiconion) echo true ;;
    *) echo false ;;
  esac
}

SUMMARY="$OUT/ci-bench-summary.md"
{
  echo "# ci-bench (loopback, shared runner — diagnostic only)"
  echo ""
  echo "conns=$CONNS duration=$DURATION commit=$(git rev-parse --short HEAD)"
  echo ""
  echo "| transport | scenario | outcome | verdict | LT delivery | staleness p99 (ms) | server CPU util | server RSS (MB) |"
  echo "|---|---|---|---|---:|---:|---:|---:|"
} >"$SUMMARY"

port=43001
failures=0
for transport in raw_udp enet kcp litenetlib websocket magiconion; do
  for scenario_name in auth room; do
    scenario_var="scenario_$scenario_name"
    dir="$OUT/$transport-$scenario_name"
    config="$dir/config.json"
    mkdir -p "$dir"
    jq -n \
      --arg transport "$transport" \
      --argjson server "$(command_json "$transport" server "$port")" \
      --argjson client "$(command_json "$transport" client "$port")" \
      --argjson scenario "${!scenario_var}" \
      --argjson sched "$(sched_is_measurand "$transport")" \
      --arg duration "$DURATION" \
      --argjson conns "$CONNS" \
      --arg out "$dir/run" \
      '{transport: $transport, server_command: $server, client_command: $client,
        client_procs: 1, total_conns: $conns, scenario: $scenario,
        warmup: "2s", duration: $duration, drain: "1s",
        staleness_period_ns: 10000000, sched_is_measurand: $sched,
        output_dir: $out}' >"$config"
    port=$((port + 1))

    echo "[ci-bench] $transport/$scenario_name" >&2
    "$ORCHESTRATOR" run -config "$config" >"$dir/orchestrator.log" 2>&1 || true
    result="$dir/run/result.json"
    if [[ ! -f "$result" ]]; then
      failures=$((failures + 1))
      echo "| $transport | $scenario_name | NO_RESULT | - | - | - | - | - |" >>"$SUMMARY"
      tail -20 "$dir/orchestrator.log" >&2
      continue
    fi
    jq -r --arg t "$transport" --arg s "$scenario_name" '
      [$t, $s, (.outcome // "?"), (.verdict // "?"),
       ((.metrics.classes.loss_tolerant.delivery_ratio // 0) | tostring | .[0:8]),
       (((.metrics.staleness_ns.p99_ns // 0) / 1000000) | tostring | .[0:8]),
       ((.cost.server.cpu_utilization // 0) | tostring | .[0:8]),
       (((.cost.server.max_rss_bytes // 0) / 1048576 | floor) | tostring)]
      | "| " + join(" | ") + " |"' "$result" >>"$SUMMARY"
  done
done

cat "$SUMMARY"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  cat "$SUMMARY" >>"$GITHUB_STEP_SUMMARY"
fi
if [[ "$failures" -gt 0 ]]; then
  echo "[ci-bench] $failures run(s) produced no result.json" >&2
  exit 1
fi
