#!/usr/bin/env bash
# A/A 判定(ADR-0005): campaign-summary.json(複数可 — 時間帯別セッションを
# 併合)から、セルごとに「ホスト内 median → ホスト間全幅 / 全体 median」を
# 計算し、5%(MDE/2)以内なら PASS。censored / measurement_invalid の block は
# 除外して開示する。ホストあたりの有効 block 数もそのまま出す(dispatch は
# job を動的に割るため、必ずしも 3 block/host に揃わない)。
#
# usage: scripts/fleet/aa-analyze.sh <campaign-summary.json>...
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: $0 <campaign-summary.json>..." >&2; exit 2; }

jq -s '
  [ .[] as $summary | $summary.jobs[] as $job | $job.cells[]
    | select((.censored // false) == false
        and (.measurement_invalid // false) == false
        and (.block_invalid // false) == false)
    | {campaign: $summary.campaign, host: $job.host, job: $job.job,
       cell: "\(.transport)/\(.scenario // .workload)/\(.regime)",
       capacity} ]
  | group_by(.cell)
  | map({
      cell: .[0].cell,
      hosts: (group_by(.host) | map({
        host: .[0].host,
        blocks: length,
        capacities: [.[].capacity],
        median: ([.[].capacity] | sort | .[(length - 1) / 2 | floor])
      })),
    }
    | .medians = [.hosts[].median]
    | .overall_median = (.medians | sort | .[(length - 1) / 2 | floor])
    | .spread = (if (.hosts | length) < 2 or .overall_median == 0 then null
        else ((.medians | max) - (.medians | min)) / .overall_median end)
    | .pass = (if .spread == null then null else .spread <= 0.05 end))
' "$@"
