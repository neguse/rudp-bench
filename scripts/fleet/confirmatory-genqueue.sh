#!/usr/bin/env bash
# screening の境界から confirmatory queue を生成する(ADR-0002 §5 / ADR-0004 §3)。
# 探索窓 = 境界 ±10%(下限 4)。block = template の sweep を窓に絞ったもの × seed。
# template には measurement_mode/baseline/doctor_report など reference mode の
# 要件を含めておくこと(本 script は conns 窓と seed だけを差し替える)。
# usage: confirmatory-genqueue.sh <screening-capacity.json> <template-sweep.json> \
#          <queue-dir> <cell-name> [n-seeds (default 3)]
set -euo pipefail
CAP="${1:?screening capacity.json}"
TEMPLATE="${2:?template sweep.json}"
QUEUE="${3:?queue dir}"
CELL="${4:?cell name}"
SEEDS="${5:-3}"

B=$(jq -r '.cells[0].capacity' "$CAP")
CENSORED=$(jq -r '.cells[0].censored // false' "$CAP")
[ "$B" != "null" ] && [ "$B" -gt 0 ] || { echo "boundary が読めません: $CAP" >&2; exit 1; }
if [ "$CENSORED" = "true" ]; then
  echo "screening が censored(境界未観測)のため confirmatory を生成しません: $CAP" >&2
  exit 1
fi
MIN=$(( B * 90 / 100 )); [ "$MIN" -ge 4 ] || MIN=4
MAX=$(( (B * 110 + 99) / 100 ))

for s in $(seq 1 "$SEEDS"); do
  d="$QUEUE/$CELL-s$s"
  mkdir -p "$d"
  jq --argjson min "$MIN" --argjson max "$MAX" --argjson seed "$s" \
     '.conns={min: $min, max: $max} | .seed=$seed | .output_dir="__JOB__/out/cell"' \
     "$TEMPLATE" > "$d/sweep.json"
  cat > "$d/block.json" <<EOF
{
  "rig": "__RIG__",
  "seed": $s,
  "output_dir": "__JOB__/out",
  "tar": true,
  "sweeps": [
    {
      "name": "$CELL",
      "config": "__JOB__/sweep.json"
    }
  ]
}
EOF
done
echo "generated: $CELL boundary=$B window=[$MIN,$MAX] seeds=$SEEDS -> $QUEUE"
