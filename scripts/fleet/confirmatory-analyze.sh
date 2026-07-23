#!/usr/bin/env bash
# confirmatory の停止規則(ADR-0004 §3、2026-07-23 凍結値)を campaign summary に適用する。
#   - cell = job 名から seed 接尾辞(-sN)を除いた単位
#   - 有効(非 censored)block の直近連続 3 個で spread=(max-min)/median を取り、
#     ≤5% なら FROZEN(値 = その 3 個の median)
#   - 3 個未満または spread>5% で総 block 数 <5 なら MORE_BLOCKS(残り数を出す)
#   - 5 block で未達なら INCONCLUSIVE
#   - censored block は統計から除外して件数を開示
# usage: confirmatory-analyze.sh <campaign-summary.json>...
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: $0 <campaign-summary.json>..." >&2; exit 2; }

jq -s '
  [.[].jobs[]] as $jobs
  | [ $jobs[] | {cell: (.job | sub("-s[0-9]+$"; "")), job, cells} ]
  | group_by(.cell)
  | map(
      . as $g
      | ($g | map(.cells[] | select(.censored != true) | .capacity) | sort) as $valid
      | ($g | map(.cells[] | select(.censored == true)) | length) as $censored_n
      | ($valid | length) as $n
      | (if $n >= 3 then ($valid[-3:]) else $valid end) as $window
      | (if $n >= 3 then
           (($window | sort) as $w
            | ($w[1]) as $med
            | (if $med > 0 then (($w[2] - $w[0]) / $med) else 1 end))
         else null end) as $spread
      | {cell: $g[0].cell,
         blocks: $n,
         censored_blocks: $censored_n,
         values: $valid,
         spread_pct: (if $spread != null then ($spread * 1000 | round / 10) else null end),
         verdict:
           (if $n >= 3 and $spread != null and $spread <= 0.05 then "FROZEN"
            elif ($n + $censored_n) >= 5 then "INCONCLUSIVE"
            else "MORE_BLOCKS"
            end),
         value: (if $n >= 3 and $spread != null and $spread <= 0.05
                 then ($window | sort | .[1]) else null end),
         need: (if $n >= 3 and $spread != null and $spread <= 0.05 then 0
                elif ($n + $censored_n) >= 5 then 0
                else (5 - $n - $censored_n) end)}
    )
' "$@"
