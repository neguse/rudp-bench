#!/usr/bin/env bash
# reference campaign(ADR-0006 stage 1)の 1 library 分の queue を生成する。
#   - screening job: wan 3 cell(auth-s1000 / auth-s4000 / room)。preset 指名、
#     conns 上限は auth 2048 / room 512(battle.md「reference campaign 手順」)
#   - confirmatory template: screening の境界確定後に confirmatory-genqueue.sh へ
#     渡す reference mode config(preset + baseline + doctor)
#   - --with-envbase: envbase(wan)の screening job と template も生成
#     (環境開示。wave で一回だけ — session 1 に相乗り)
# usage: ref-genqueue.sh <transports/lib.json> <queue-dir> [--with-envbase]
set -euo pipefail
TRANSPORT="${1:?transports/<lib>.json}"
QUEUE="${2:?queue dir}"
WITH_ENVBASE=0
[ "${3:-}" = "--with-envbase" ] && WITH_ENVBASE=1

ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
RIG_JSON="$ROOT/orchestrator/rigs/aws-c8g-16xlarge.json"
PROBE_EXAMPLE="$ROOT/orchestrator/examples/class-mapping-conformance-local.json"

TNAME=$(jq -r .name "$TRANSPORT")
TSPEC=$(jq .spec "$TRANSPORT")

# block 前後 baseline(drift gate)。probe 点 c128 は drift 許容幅の導出に使った
# envbase PASS 域(session 3 の c4-c128)。drift 値は凍結値(run.ConfirmatoryV1)
# と一致しないと sweep config が load で弾かれる
BASELINE=$(cat <<'EOF'
{
  "transport": "raw_udp",
  "server_command": ["build-v2-rawudp/raw_udp_server", "--port", "42916"],
  "client_command": [
    "build-v2-rawudp/raw_udp_client", "--host", "10.200.0.1", "--port", "42916",
    "--conns", "{conns}", "--proc-index", "{proc_index}",
    "--origin-base", "{origin_id_start}"
  ],
  "client_procs": 8,
  "scenario": {
    "name": "ref-env-baseline-wan",
    "kind": "environment_baseline",
    "client_input": {
      "traffic_id": 1,
      "loss_tolerant": {
        "rate_hz": 100, "payload_bytes": 64,
        "staleness_p99_ns": 300000000, "min_delivery_ratio": 0.95
      },
      "must_deliver": {"rate_hz": 0, "payload_bytes": 0}
    }
  },
  "total_conns": 128,
  "warmup": "25s", "duration": "60s", "drain": "5s",
  "drift": {"max_delivery_delta": 0.010, "max_staleness_p99_ratio": 1.10}
}
EOF
)

emit_job() { # $1=job名 $2=sweep.json 中身
  local d="$QUEUE/$1"
  mkdir -p "$d"
  echo "$2" | jq . > "$d/sweep.json"
  jq -n --arg name "$1" '{
    rig: "__RIG__", seed: 1, output_dir: "__JOB__/out", tar: true,
    sweeps: [{name: $name, config: "__JOB__/sweep.json"}]
  }' > "$d/block.json"
}

emit_cell() { # $1=cell 略名 $2=preset 名 $3=conns 上限
  local cell="$1" preset="$2" max="$3"
  # screening: N=1 の粗い探索。preset が条件を固定する
  emit_job "$TNAME-scr-$cell" "$(jq -n \
    --arg tname "$TNAME" --argjson tspec "$TSPEC" \
    --arg preset "$preset" --argjson max "$max" '{
      measurement_mode: "screening", regime: "ref-wan", preset: $preset,
      transports: {($tname): $tspec},
      conns: {min: 4, max: $max}, seed: 1, output_dir: "__JOB__/out/cell"
    }')"
  # confirmatory template: conns 窓と seed は confirmatory-genqueue.sh が差し替える
  jq -n --arg tname "$TNAME" --argjson tspec "$TSPEC" \
    --arg preset "$preset" --argjson baseline "$BASELINE" '{
      measurement_mode: "reference", regime: "ref-wan", preset: $preset,
      doctor_report: "__JOB__/out/doctor.json",
      baseline: $baseline,
      transports: {($tname): $tspec},
      conns: {min: 4, max: 512}, seed: 1, output_dir: "__JOB__/out/cell"
    }' > "$QUEUE/confirm-$TNAME-$cell.tmpl.json"
}

mkdir -p "$QUEUE"
emit_cell auth1000 ref-auth-wan-s1000-v1 2048
emit_cell auth4000 ref-auth-wan-s4000-v1 2048
emit_cell room ref-room-wan-v1 512

# class-mapping probe(session 冒頭に host 1 台で実行し clean 証跡へ昇格)。
# probe の器差を避けるため条件は local example と同一、CPU だけ rig に合わせる
jq --arg t "$TNAME" \
   --arg scpus "$(jq -r .server_cpus "$RIG_JSON")" \
   --arg ccpus "$(jq -r .client_cpus "$RIG_JSON")" '{
  version, probe,
  server_cpus: $scpus, client_cpus: $ccpus,
  output_dir: ("results-v2/ref1-probe-" + $t),
  transports: {raw_udp: .transports.raw_udp, ($t): .transports[$t]}
}' "$PROBE_EXAMPLE" > "$QUEUE/probe-$TNAME.json"

if [ "$WITH_ENVBASE" = 1 ]; then
  # envbase は preset 外(環境開示 cell)。条件は preset と同値を明示する
  ENV_COMMON=$(jq -n --argjson baseline "$BASELINE" '{
    regime: "ref-wan",
    netem: {
      link_mtu_bytes: 1500, disable_offloads: true,
      server_egress: {delay_ms: 25, loss_pct: 1},
      client_egress: {delay_ms: 25, loss_pct: 1}
    },
    scenarios: [$baseline.scenario],
    transports: {raw_udp: {
      server_command: $baseline.server_command,
      client_command: $baseline.client_command,
      client_procs: $baseline.client_procs
    }},
    warmup: "25s", steady_warmup: true, duration: "60s", drain: "5s",
    staleness_period_ns: 10000000,
    seed: 1, output_dir: "__JOB__/out/cell"
  }')
  emit_job "envbase-scr" "$(echo "$ENV_COMMON" | jq \
    '. + {measurement_mode: "screening", conns: {min: 4, max: 2048}}')"
  echo "$ENV_COMMON" | jq --argjson baseline "$BASELINE" \
    '. + {measurement_mode: "reference", doctor_report: "__JOB__/out/doctor.json",
          baseline: $baseline, conns: {min: 4, max: 512}}' \
    > "$QUEUE/confirm-envbase.tmpl.json"
fi

echo "generated: $QUEUE(transport=$TNAME, envbase=$WITH_ENVBASE)"
