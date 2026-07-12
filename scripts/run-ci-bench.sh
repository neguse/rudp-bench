#!/usr/bin/env bash
set -euo pipefail

# CI 用の軽量 loopback ramp ベンチ。接続数を1 run内で単調に増やし、
# traffic 別 SLO が最初に割れた target connection 数を診断 score にする。
# 共有 runner 上の値であり、reference・推薦には使わない(隔離なし・
# netem なし・N=1、ramp-rate 依存)。結果ディレクトリは
# Actions artifact として配布し、repo へは追加しない。
# 各 run の outcome は summary に記録するが、SUT の FAIL/INVALID では
# 終了コードを立てない(CI の合否は既存 smoke が担う)。result.json が
# 生成されない場合だけ失敗させる。

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

ORCHESTRATOR="${ORCHESTRATOR:-build-v2/orchestrator}"
OUT="${OUT:-results-v2/ci-bench}"
AUTH_START_CONNS="${AUTH_START_CONNS:-16}"
AUTH_STEP_CONNS="${AUTH_STEP_CONNS:-64}"
AUTH_MAX_CONNS="${AUTH_MAX_CONNS:-512}"
ROOM_START_CONNS="${ROOM_START_CONNS:-8}"
ROOM_STEP_CONNS="${ROOM_STEP_CONNS:-16}"
ROOM_MAX_CONNS="${ROOM_MAX_CONNS:-128}"
RAMP_GUARD="${RAMP_GUARD:-250ms}"
RAMP_SAMPLE="${RAMP_SAMPLE:-1s}"
RAMP_DRAIN="${RAMP_DRAIN:-250ms}"

rm -rf "$OUT"
mkdir -p "$OUT"

scenario_auth='{
  "name": "ci-auth", "kind": "authoritative_state",
  "client_input": {
    "traffic_id": 1,
    "loss_tolerant": {"rate_hz": 20, "payload_bytes": 64, "staleness_p99_ns": 300000000, "min_delivery_ratio": 0.9},
    "must_deliver": {"rate_hz": 10, "payload_bytes": 64, "deadline_ns": 200000000, "min_deadline_hit_ratio": 0.9, "min_eventual_delivery_ratio": 0.99}
  },
  "server_state": {
    "traffic_id": 2,
    "loss_tolerant": {"rate_hz": 20, "payload_bytes": 256, "staleness_p99_ns": 300000000, "min_delivery_ratio": 0.9},
    "must_deliver": {"rate_hz": 10, "payload_bytes": 64, "deadline_ns": 200000000, "min_deadline_hit_ratio": 0.9, "min_eventual_delivery_ratio": 0.99}
  }
}'
scenario_room='{
  "name": "ci-room", "kind": "room_relay",
  "room_publish": {
    "traffic_id": 3,
    "loss_tolerant": {"rate_hz": 20, "payload_bytes": 128, "staleness_p99_ns": 300000000, "min_delivery_ratio": 0.9},
    "must_deliver": {"rate_hz": 10, "payload_bytes": 64, "deadline_ns": 200000000, "min_deadline_hit_ratio": 0.9, "min_eventual_delivery_ratio": 0.99}
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
  echo "# ci-ramp-bench (loopback, shared runner — diagnostic only)"
  echo ""
  echo "auth=${AUTH_START_CONNS}..${AUTH_MAX_CONNS}/+${AUTH_STEP_CONNS}, room=${ROOM_START_CONNS}..${ROOM_MAX_CONNS}/+${ROOM_STEP_CONNS}, guard=${RAMP_GUARD}, sample=${RAMP_SAMPLE}, drain=${RAMP_DRAIN}, commit=$(git rev-parse --short HEAD)"
  echo ""
  echo "| transport | scenario | outcome | verdict | ramp score | min delivery | max staleness p99 (ms) | min deadline hit | first violation |"
  echo "|---|---|---|---|---:|---:|---:|---:|---|"
} >"$SUMMARY"

port=43001
failures=0
for transport in raw_udp enet kcp litenetlib websocket magiconion; do
  for scenario_name in auth room; do
    scenario_var="scenario_$scenario_name"
    if [[ "$scenario_name" == auth ]]; then
      start_conns="$AUTH_START_CONNS"
      step_conns="$AUTH_STEP_CONNS"
      max_conns="$AUTH_MAX_CONNS"
    else
      start_conns="$ROOM_START_CONNS"
      step_conns="$ROOM_STEP_CONNS"
      max_conns="$ROOM_MAX_CONNS"
    fi
    dir="$OUT/$transport-$scenario_name"
    config="$dir/config.json"
    mkdir -p "$dir"
    jq -n \
      --arg transport "$transport" \
      --argjson server "$(command_json "$transport" server "$port")" \
      --argjson client "$(command_json "$transport" client "$port")" \
      --argjson scenario "${!scenario_var}" \
      --argjson sched "$(sched_is_measurand "$transport")" \
      --argjson start_conns "$start_conns" \
      --argjson step_conns "$step_conns" \
      --argjson max_conns "$max_conns" \
      --arg guard "$RAMP_GUARD" \
      --arg sample "$RAMP_SAMPLE" \
      --arg ramp_drain "$RAMP_DRAIN" \
      --arg out "$dir/run" \
      '{transport: $transport, server_command: $server, client_command: $client,
        client_procs: 1, total_conns: $max_conns, scenario: $scenario,
        ramp: {start_conns: $start_conns, step_conns: $step_conns,
               guard: $guard, sample: $sample, drain: $ramp_drain},
        warmup: "1s", drain: "500ms",
        staleness_period_ns: 10000000, sched_is_measurand: $sched,
        output_dir: $out}' >"$config"
    port=$((port + 1))

    echo "[ci-ramp] $transport/$scenario_name" >&2
    "$ORCHESTRATOR" run -config "$config" >"$dir/orchestrator.log" 2>&1 || true
    result="$dir/run/result.json"
    if [[ ! -f "$result" ]]; then
      failures=$((failures + 1))
      echo "| $transport | $scenario_name | NO_RESULT | - | - | - | - | - | - |" >>"$SUMMARY"
      tail -20 "$dir/orchestrator.log" >&2
      continue
    fi
    jq -r --arg t "$transport" --arg s "$scenario_name" '
      . as $result
      | (($result.ramp.timeline // [])
          | map(select(.evaluation.ok == false))
          | .[0] // ($result.ramp.timeline[-1] // null)) as $point
      | [($point.evaluation.traffic // [])[] | .delivery_ratio] as $delivery
      | [($point.evaluation.traffic // [])[] | select(.class == "loss_tolerant") | .staleness_p99_ns] as $staleness
      | [($point.evaluation.traffic // [])[] | select(.class == "must_deliver") | .deadline_hit_ratio] as $deadline
      | [$t, $s, (.outcome // "?"), (.verdict // "?"),
       (if .verdict != "VALID" or .ramp == null then "-"
        elif .ramp.censored then "≥" + (.config.total_conns | tostring)
        else (.ramp.score_conns | tostring) end),
       (if .verdict != "VALID" or ($delivery | length) == 0 then "-" else ($delivery | min | tostring) end),
       (if .verdict != "VALID" or ($staleness | length) == 0 then "-" else (($staleness | max) / 1000000 | tostring) end),
       (if .verdict != "VALID" or ($deadline | length) == 0 then "-" else ($deadline | min | tostring) end),
       (if .verdict != "VALID"
        then (if ((.invalid_reasons // []) | length) > 0
            then ((.invalid_reasons // []) | join("; "))
            else "-"
            end)
        else (.ramp.cause // "-")
        end)]
      | "| " + join(" | ") + " |"' "$result" >>"$SUMMARY"
  done
done

{
  echo ""
  echo "## Ramp score ranking (higher is better)"
  for scenario_name in auth room; do
    echo ""
    echo "### $scenario_name"
    echo ""
    find "$OUT" -path "*-$scenario_name/run/result.json" -print0 \
      | xargs -0 -r jq -s -r '
          sort_by(if .verdict != "VALID" or .ramp == null
                  then [1, 0, 0, .transport]
                  else [0,
                        -(.ramp.score_conns // .config.total_conns),
                        -(if .ramp.censored then 1 else 0 end),
                        .transport]
                  end)
          | to_entries[]
          | "\(.key + 1). \(.value.transport): "
            + (if .value.verdict != "VALID" or .value.ramp == null then "INVALID"
               elif .value.ramp.censored then "≥\(.value.config.total_conns)"
               else "\(.value.ramp.score_conns)" end)
            + (if .value.transport == "raw_udp" then " (reference)" else "" end)'
  done
} >>"$SUMMARY"

cat "$SUMMARY"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  cat "$SUMMARY" >>"$GITHUB_STEP_SUMMARY"
fi
if [[ "$failures" -gt 0 ]]; then
  echo "[ci-ramp] $failures run(s) produced no result.json" >&2
  exit 1
fi
