#!/usr/bin/env bash
# fleet campaign coordinator(ADR-0005 enabler #3)。
# ローカルマシンで実行し、AWS 側は campaign-scoped な ephemeral リソースのみ
# (IaC なし。安全網は tag: Project/Campaign → cleanup が全削除)。
# ホストは credential を持たず、配布・回収は SSH/scp のみ。
#
# usage:
#   scripts/fleet/campaign.sh launch    -n <hosts> [-campaign <id>] [-commit <sha7>]
#   scripts/fleet/campaign.sh status    -campaign <id>
#   scripts/fleet/campaign.sh dispatch  -campaign <id> -queue <dir> [-deadline-min <N>]
#   scripts/fleet/campaign.sh aggregate -campaign <id>
#   scripts/fleet/campaign.sh cleanup   -campaign <id>
#
# queue 形式: <dir>/<job>/block.json または run.json(+ 参照 config)。
# config 内の __JOB__ は dispatch がホスト側 job dir(campaign-jobs/<job>)へ
# 置換する。output_dir は "__JOB__/out" を指すこと(回収対象 = out.tar.gz。
# block は自前 tar、run は dispatch が remote で tar する)。
# 配布・retry の単位は job = 1 block/run(ADR-0005: 中断 block は丸ごと捨てる)。
# run job は SLO 破断(rc=3)・censored(rc=5)もデータとして受理し、
# INVALID(rc=2)/unsupported(rc=4)だけを失敗扱いにする。
#
# TODO(A/A 設計時): raw_udp anchor の fleet median gate。boot.sh 側の anchor
# probe 追加とセットで入れる。IMDS interruption notice の先読み requeue も
# 当面は「SSH 不達 = ホスト死亡 → requeue」の事後検知で代替する。
set -euo pipefail
cd "$(dirname "$0")/../.."

AWS=${AWS_CLI:-aws}
command -v "$AWS" >/dev/null || AWS="$HOME/.local/share/mise/installs/awscli/latest/.mise-bins/aws"

CONFIG=scripts/fleet/config.json
REGION=$(jq -r .region $CONFIG)
AMI=$(jq -r .ami_id $CONFIG)
ITYPE=$(jq -r .instance_type $CONFIG)
RIG=$(jq -r .rig $CONFIG)
RELEASE_TAG=$(jq -r .bundle_release_tag $CONFIG)
PROJECT=$(jq -r .tag_project $CONFIG)

aws2() { "$AWS" --region "$REGION" --output json --no-cli-pager "$@"; }
tagspec() { # $1=resource-type $2=campaign
  echo "ResourceType=$1,Tags=[{Key=Project,Value=$PROJECT},{Key=Campaign,Value=$2}]"
}

CMD=${1:-}; shift || true
HOSTS=1; CAMPAIGN=""; COMMIT=""; QUEUE=""; DEADLINE_MIN=30
while [ $# -gt 0 ]; do
  case "$1" in
    -n) HOSTS="$2"; shift 2 ;;
    -campaign) CAMPAIGN="$2"; shift 2 ;;
    -commit) COMMIT="$2"; shift 2 ;;
    -queue) QUEUE="$2"; shift 2 ;;
    -deadline-min) DEADLINE_MIN="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

WORKDIR_ROOT="results-v2/fleet"
WORKDIR="$WORKDIR_ROOT/$CAMPAIGN"

# --- SSH ------------------------------------------------------------------
# fleet ホストは per-campaign key のみで認証する。known_hosts も campaign に
# 閉じる(ephemeral ホストの鍵をグローバルに残さない)
ssh_opts() {
  echo "-i $WORKDIR/ssh-key -o UserKnownHostsFile=$WORKDIR/known_hosts \
    -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -o BatchMode=yes \
    -o LogLevel=ERROR"
}
# shellcheck disable=SC2046
fssh() { local ip="$1"; shift; ssh $(ssh_opts) "ubuntu@$ip" "$@"; }
# shellcheck disable=SC2046
fscp() { scp $(ssh_opts) "$@"; }

running_ips() {
  aws2 ec2 describe-instances \
    --filters "Name=tag:Project,Values=$PROJECT" "Name=tag:Campaign,Values=$CAMPAIGN" \
      "Name=instance-state-name,Values=running" \
    | jq -r '.Reservations[].Instances[].PublicIpAddress // empty'
}

# boot gate の完了を待つ。合否は /var/lib/rudp-bench/boot-gate/{READY,FAILED}
# (user-data 参照)。gate 証跡はホスト単位で回収して手元に残す
BOOT_TIMEOUT_S=900
wait_boot_gate() { # $1=ip → 0:READY 1:FAILED/timeout
  local ip="$1" waited=0
  while [ "$waited" -lt "$BOOT_TIMEOUT_S" ]; do
    if fssh "$ip" 'test -f /var/lib/rudp-bench/boot-gate/READY' 2>/dev/null; then
      mkdir -p "$WORKDIR/hosts/$ip"
      fscp -r "ubuntu@$ip:/var/lib/rudp-bench/boot-gate" "$WORKDIR/hosts/$ip/" 2>/dev/null || true
      return 0
    fi
    if fssh "$ip" 'test -f /var/lib/rudp-bench/boot-gate/FAILED' 2>/dev/null; then
      mkdir -p "$WORKDIR/hosts/$ip"
      fscp -r "ubuntu@$ip:/var/lib/rudp-bench/boot-gate" "$WORKDIR/hosts/$ip/" 2>/dev/null || true
      fscp "ubuntu@$ip:/var/log/rudp-boot-gate.log" "$WORKDIR/hosts/$ip/" 2>/dev/null || true
      echo "boot gate FAILED: $ip(証跡: $WORKDIR/hosts/$ip)" >&2
      return 1
    fi
    sleep 15; waited=$((waited + 15))
  done
  echo "boot gate timeout(${BOOT_TIMEOUT_S}s): $ip" >&2
  return 1
}

# --- job queue ------------------------------------------------------------
# state は workdir 内のディレクトリ移動で管理する(mv の原子性で claim)。
#   queue/pending/<job>/ → queue/running/<job>/ → queue/done/<job>/
#                                               → queue/failed/<job>/
MAX_ATTEMPTS=2

claim_job() { # → job 名(なければ空)
  local d job
  for d in "$WORKDIR"/queue/pending/*/; do
    [ -d "$d" ] || continue
    job=$(basename "$d")
    if mv "$d" "$WORKDIR/queue/running/$job" 2>/dev/null; then
      echo "$job"; return 0
    fi
  done
  return 0
}

run_job() { # $1=ip $2=job → 0:成功(回収済み) 1:失敗 255:ホスト不達
  local ip="$1" job="$2" hostdir="campaign-jobs/$job"
  local jobout="$WORKDIR/jobs/$job"
  mkdir -p "$jobout"

  # render: __JOB__ をホスト側 path、__RIG__ を config.json の rig に置換した
  # コピーを配布する(同じ queue を instance size 違いの campaign で使い回す)
  rm -rf "$jobout/rendered"
  cp -r "$WORKDIR/queue/running/$job" "$jobout/rendered"
  find "$jobout/rendered" -name '*.json' -exec sed -i -e "s|__JOB__|$hostdir|g" -e "s|__RIG__|$RIG|g" {} +

  fssh "$ip" "sudo rm -rf /opt/rudp-bench/$hostdir && sudo install -d -o ubuntu /opt/rudp-bench/campaign-jobs /opt/rudp-bench/$hostdir" || return $?
  fscp -r "$jobout/rendered/." "ubuntu@$ip:/opt/rudp-bench/$hostdir/" || return $?

  # 前 job 異常終了の netns 残留を冪等に掃除(run-sweep.sh と同じ罠)。
  # bench.slice の service として実行(scope は LimitNOFILE を持てない)
  local bench_cpus kind remote_cmd
  bench_cpus=$(jq -r .bench_cpus "$RIG")
  if [ -f "$jobout/rendered/run.json" ]; then kind=run; else kind=block; fi
  # 計測器(orchestrator 本体と netem evidence 採取の exec)を SUT と別 CPU へ
  # 隔離する。共有だと過負荷点で evidence capture が starvation spike を食い、
  # 窓終端をはみ出して INVALID が flap する(c20260718-060820 の c64/ramp で実証)
  local pin=""
  if [ "$kind" = run ]; then
    pin=$(python3 - "$RIG" "$jobout/rendered/run.json" <<'PY'
import json, sys

def parse(spec):
    cpus = set()
    for part in spec.split(","):
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-")
            cpus |= set(range(int(lo), int(hi) + 1))
        else:
            cpus.add(int(part))
    return cpus

rig = json.load(open(sys.argv[1]))
run = json.load(open(sys.argv[2]))
free = parse(rig["bench_cpus"]) - parse(run.get("server_cpus", "")) - parse(run.get("client_cpus", ""))
print(",".join(str(c) for c in sorted(free)))
PY
)
  fi
  local orch="/opt/rudp-bench/build-v2/orchestrator"
  [ -n "$pin" ] && orch="taskset -c $pin $orch"
  remote_cmd="sudo ip netns del rudpbench-srv 2>/dev/null; sudo ip netns del rudpbench-cli 2>/dev/null; \
    sudo systemd-run --wait --pipe --collect --unit='rudp-job-$job' --slice=bench.slice \
      -p AllowedCPUs='$bench_cpus' -p CPUWeight=10000 -p LimitNOFILE=1048576 \
      -p WorkingDirectory=/opt/rudp-bench \
      $orch $kind -config '$hostdir/$kind.json'"
  if [ "$kind" = run ]; then
    # run は非 PASS でも result を残して非ゼロ exit する。破断(3)と
    # inconclusive/censored(5)は ramp のデータなので受理し、tar も自前で作る
    remote_cmd="$remote_cmd; rc=\$?; case \$rc in 0|3|5) ;; *) exit \$rc ;; esac; \
      sudo tar czf '/opt/rudp-bench/$hostdir/out.tar.gz' -C '/opt/rudp-bench/$hostdir' out"
  fi
  local rc=0
  fssh "$ip" "$remote_cmd" > "$jobout/run.log" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "job $job on $ip: $kind rc=$rc(log: $jobout/run.log)" >&2
    # 失敗 run の証跡(result.json 等)を診断用に回収する。retry の rm -rf や
    # cleanup の terminate で消える前に手元へ残す(attempt 別に保持)
    local attempt=$(( $(cat "$WORKDIR/queue/running/$job/.attempts" 2>/dev/null || echo 0) + 1 ))
    fscp -r "ubuntu@$ip:/opt/rudp-bench/$hostdir/out" "$jobout/diag-attempt-$attempt" 2>/dev/null || true
    return "$rc"
  fi

  # 逐次回収(ADR-0005: campaign 末尾一括だと spot 中断でロストする)
  fscp "ubuntu@$ip:/opt/rudp-bench/$hostdir/out.tar.gz" "$jobout/" || return $?
  tar -tzf "$jobout/out.tar.gz" > /dev/null || { echo "job $job: 回収 tar が壊れている" >&2; return 1; }
  echo "$ip" > "$jobout/host"
  return 0
}

host_worker() { # $1=ip $2=deadline_epoch
  local ip="$1" deadline="$2" job rc attempts
  if ! wait_boot_gate "$ip"; then
    echo "worker $ip: boot gate 非 PASS のため離脱(job は他ホストが拾う)" >&2
    return 0
  fi
  # farm 凍結構成の前提 sysctl(ledger #5: client rcvbuf 4MB。tuned enet client
  # は効かないと fail fast する)。本来は boot.sh(bundle)の仕事 — ledger #24
  if ! fssh "$ip" 'sudo sysctl -q -w net.core.rmem_max=8388608 net.core.wmem_max=8388608'; then
    echo "worker $ip: host prep(sysctl)失敗 → 離脱" >&2
    return 0
  fi
  echo "worker $ip: READY(gate 証跡回収済み)"
  while :; do
    if [ "$(date +%s)" -ge "$deadline" ]; then
      echo "worker $ip: 打ち切り時刻に到達" >&2
      return 0
    fi
    job=$(claim_job)
    if [ -z "$job" ]; then
      # pending が空でも、他ホストの running job が失敗して requeue される
      # 可能性が残る間は待つ(全 running が消えたら campaign 終了)
      if [ "$(find "$WORKDIR/queue/running" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 0 ]; then
        echo "worker $ip: queue 空"; return 0
      fi
      sleep 20; continue
    fi
    echo "worker $ip: job $job 開始"
    rc=0; run_job "$ip" "$job" || rc=$?
    if [ "$rc" -eq 0 ]; then
      mv "$WORKDIR/queue/running/$job" "$WORKDIR/queue/done/$job"
      echo "worker $ip: job $job 完了・回収済み"
      continue
    fi
    # 失敗: attempt を数えて requeue(上限で failed へ)。SSH 不達(255)は
    # spot 中断とみなし、job を requeue してこの worker は離脱する
    attempts=$(( $(cat "$WORKDIR/queue/running/$job/.attempts" 2>/dev/null || echo 0) + 1 ))
    echo "$attempts" > "$WORKDIR/queue/running/$job/.attempts"
    if [ "$rc" -eq 255 ]; then
      mv "$WORKDIR/queue/running/$job" "$WORKDIR/queue/pending/$job"
      echo "worker $ip: SSH 不達 → job $job を requeue して離脱(spot 中断?)" >&2
      return 0
    fi
    if [ "$attempts" -ge "$MAX_ATTEMPTS" ]; then
      mv "$WORKDIR/queue/running/$job" "$WORKDIR/queue/failed/$job"
      echo "worker $ip: job $job は $attempts 回失敗 → failed" >&2
    else
      mv "$WORKDIR/queue/running/$job" "$WORKDIR/queue/pending/$job"
      echo "worker $ip: job $job requeue(attempt $attempts)" >&2
    fi
  done
}

case "$CMD" in
launch)
  CAMPAIGN=${CAMPAIGN:-"c$(date +%Y%m%d-%H%M%S)"}
  COMMIT=${COMMIT:-$(git rev-parse --short=7 HEAD)}
  WORKDIR="$WORKDIR_ROOT/$CAMPAIGN"
  mkdir -p "$WORKDIR"
  BUNDLE_URL="https://github.com/neguse/rudp-bench/releases/download/$RELEASE_TAG/bundle-$COMMIT.tar.zst"
  # bundle の実在を先に確認(fleet を建ててから 404 に気づかない)
  curl -fsIL -o /dev/null "$BUNDLE_URL" || { echo "ERROR: bundle が無い: $BUNDLE_URL" >&2; exit 1; }
  echo "$COMMIT" > "$WORKDIR/commit"

  # per-campaign SSH key pair
  ssh-keygen -t ed25519 -N "" -q -f "$WORKDIR/ssh-key"
  aws2 ec2 import-key-pair --key-name "$PROJECT-$CAMPAIGN" \
    --public-key-material "fileb://$WORKDIR/ssh-key.pub" \
    --tag-specifications "$(tagspec key-pair "$CAMPAIGN")" > /dev/null

  # coordinator の現 IP からの SSH のみ許す SG(default VPC)
  MYIP=$(curl -fsS https://checkip.amazonaws.com)
  SG_ID=$(aws2 ec2 create-security-group --group-name "$PROJECT-$CAMPAIGN" \
    --description "rudp-bench fleet $CAMPAIGN" \
    --tag-specifications "$(tagspec security-group "$CAMPAIGN")" | jq -r .GroupId)
  aws2 ec2 authorize-security-group-ingress --group-id "$SG_ID" \
    --protocol tcp --port 22 --cidr "$MYIP/32" > /dev/null

  # user-data を render
  sed -e "s|__BUNDLE_URL__|$BUNDLE_URL|" -e "s|__RIG__|$RIG|" \
    scripts/fleet/user-data.yaml.tmpl > "$WORKDIR/user-data.yaml"

  # spot fleet(one-time、ADR-0005: on-demand fallback なし)
  aws2 ec2 run-instances \
    --image-id "$AMI" --instance-type "$ITYPE" --count "$HOSTS" \
    --key-name "$PROJECT-$CAMPAIGN" --security-group-ids "$SG_ID" \
    --instance-market-options 'MarketType=spot,SpotOptions={SpotInstanceType=one-time,InstanceInterruptionBehavior=terminate}' \
    --user-data "file://$WORKDIR/user-data.yaml" \
    --tag-specifications "$(tagspec instance "$CAMPAIGN")" \
    | jq -r '.Instances[].InstanceId' > "$WORKDIR/instance-ids"
  echo "campaign=$CAMPAIGN hosts=$HOSTS commit=$COMMIT sg=$SG_ID"
  echo "workdir=$WORKDIR"
  ;;

status)
  [ -n "$CAMPAIGN" ] || { echo "-campaign required" >&2; exit 2; }
  aws2 ec2 describe-instances \
    --filters "Name=tag:Project,Values=$PROJECT" "Name=tag:Campaign,Values=$CAMPAIGN" \
    | jq -r '.Reservations[].Instances[] | "\(.InstanceId) \(.State.Name) \(.PublicIpAddress // "-")"'
  if [ -d "$WORKDIR/queue" ]; then
    for st in pending running done failed; do
      printf '%s: %s\n' "$st" "$(ls "$WORKDIR/queue/$st" 2>/dev/null | tr '\n' ' ')"
    done
  fi
  ;;

dispatch)
  [ -n "$CAMPAIGN" ] || { echo "-campaign required" >&2; exit 2; }
  [ -n "$QUEUE" ] && [ -d "$QUEUE" ] || { echo "-queue <dir> required" >&2; exit 2; }
  [ -f "$WORKDIR/ssh-key" ] || { echo "workdir が無い(launch 済みか): $WORKDIR" >&2; exit 2; }

  # queue seed(再 dispatch は既存 state を引き継ぐ。pending へ戻すのは
  # running に残った前回の中断分のみ — done/failed は繰り返さない)
  mkdir -p "$WORKDIR"/queue/{pending,running,done,failed} "$WORKDIR/jobs"
  for d in "$QUEUE"/*/; do
    [ -d "$d" ] || continue
    job=$(basename "$d")
    if [ ! -e "$WORKDIR/queue/pending/$job" ] && [ ! -e "$WORKDIR/queue/running/$job" ] \
      && [ ! -e "$WORKDIR/queue/done/$job" ] && [ ! -e "$WORKDIR/queue/failed/$job" ]; then
      cp -r "$d" "$WORKDIR/queue/pending/$job"
    fi
  done
  for d in "$WORKDIR"/queue/running/*/; do
    [ -d "$d" ] || continue
    mv "$d" "$WORKDIR/queue/pending/$(basename "$d")"
  done

  # 打ち切り時刻(pre-registration)。超えたら未取得 cell を穴として終える
  DEADLINE_EPOCH=$(( $(date +%s) + DEADLINE_MIN * 60 ))
  date -d "@$DEADLINE_EPOCH" +%FT%T%z > "$WORKDIR/deadline"
  echo "dispatch: deadline $(cat "$WORKDIR/deadline")(${DEADLINE_MIN}分)"

  IPS=$(running_ips)
  [ -n "$IPS" ] || { echo "running なホストが居ない" >&2; exit 1; }
  pids=()
  for ip in $IPS; do
    host_worker "$ip" "$DEADLINE_EPOCH" &
    pids+=($!)
  done
  rc=0
  for pid in "${pids[@]}"; do wait "$pid" || rc=1; done

  echo "--- dispatch 結果 ---"
  for st in done failed pending running; do
    printf '%s: %s\n' "$st" "$(ls "$WORKDIR/queue/$st" 2>/dev/null | tr '\n' ' ')"
  done
  # 打ち切りで残った pending / 失敗 job は campaign の穴(原因調査の対象)
  leftover=$(find "$WORKDIR/queue/pending" "$WORKDIR/queue/failed" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
  [ "$leftover" -eq 0 ] || { echo "WARN: 未完了 job が残っている(穴)" >&2; exit 3; }
  exit "$rc"
  ;;

aggregate)
  [ -n "$CAMPAIGN" ] || { echo "-campaign required" >&2; exit 2; }
  [ -d "$WORKDIR/queue/done" ] || { echo "done な job が無い" >&2; exit 1; }

  AGG="$WORKDIR/aggregate"
  rm -rf "$AGG"; mkdir -p "$AGG"
  for d in "$WORKDIR"/queue/done/*/; do
    [ -d "$d" ] || continue
    job=$(basename "$d")
    mkdir -p "$AGG/$job"
    tar -xzf "$WORKDIR/jobs/$job/out.tar.gz" -C "$AGG/$job"
  done

  # campaign summary(機械可読)。capacity セルは block 内の各 sweep から収集
  jq -n \
    --arg campaign "$CAMPAIGN" \
    --arg commit "$(cat "$WORKDIR/commit" 2>/dev/null || echo unknown)" \
    --arg deadline "$(cat "$WORKDIR/deadline" 2>/dev/null || echo -)" \
    --slurpfile hosts <(
      for h in "$WORKDIR"/hosts/*/; do
        [ -d "$h" ] || continue
        ip=$(basename "$h")
        jq -n --arg ip "$ip" \
          --argjson doctor "$(jq '.ok' "$h/boot-gate/doctor.json" 2>/dev/null || echo null)" \
          --argjson calibration "$(ls "$h"/boot-gate/duration-invariance/attempt-* 2>/dev/null | wc -l)" \
          --argjson calibration_passed "$(jq -s 'map(.passed) | any' "$h"/boot-gate/duration-invariance/attempt-*/summary.json 2>/dev/null || echo null)" \
          --argjson anchor "$(jq '{outcome, staleness_p99_ms: (.metrics.classes.loss_tolerant.update_gap_ns.p99_ns / 1e6)}' "$h/boot-gate/anchor/result.json" 2>/dev/null || echo null)" \
          '{ip: $ip, doctor_ok: $doctor, calibration_attempts: $calibration, calibration_passed: $calibration_passed, anchor: $anchor}'
      done) \
    --slurpfile jobs <(
      for d in "$WORKDIR"/queue/done/*/; do
        [ -d "$d" ] || continue
        job=$(basename "$d")
        cells=$(find "$WORKDIR/aggregate/$job/out" -name capacity.json \
          -exec jq '.cells' {} + 2>/dev/null | jq -s 'add // []')
        # run job(ramp 等)は out/result.json が直接の成果物
        ramp=$(jq '{transport: .config.transport, workload: .config.workload,
            outcome, verdict, score_conns: .ramp.score_conns,
            censored: .ramp.censored, cause: .ramp.cause}' \
          "$WORKDIR/aggregate/$job/out/result.json" 2>/dev/null || echo null)
        jq -n --arg job "$job" --arg host "$(cat "$WORKDIR/jobs/$job/host" 2>/dev/null || echo -)" \
          --argjson attempts "$(( $(cat "$WORKDIR/queue/done/$job/.attempts" 2>/dev/null || echo 0) + 1 ))" \
          --argjson cells "$cells" --argjson ramp "$ramp" \
          '{job: $job, host: $host, attempts: $attempts, cells: $cells, ramp: $ramp}'
      done) \
    --slurpfile holes <(
      for st in pending failed; do
        for d in "$WORKDIR"/queue/$st/*/; do
          [ -d "$d" ] || continue
          jq -n --arg job "$(basename "$d")" --arg state "$st" '{job: $job, state: $state}'
        done
      done) \
    '{campaign: $campaign, commit: $commit, deadline: $deadline,
      hosts: $hosts, jobs: $jobs, holes: $holes}
     # anchor gate: fleet median ±10%(暫定幅 — A/A 完了時に凍結)+ PASS 必須。
     # probe の無い旧 bundle の campaign では null のまま(未判定)
     | ([.hosts[].anchor.staleness_p99_ms | select(. != null)] | sort) as $vals
     | .anchor_fleet_median_ms = (if ($vals | length) == 0 then null else $vals[(($vals | length) - 1) / 2 | floor] end)
     | .anchor_fleet_median_ms as $med
     | .hosts = [.hosts[] | .anchor_ok =
         (if .anchor == null or $med == null then null
          else (.anchor.outcome == "PASS"
                and (.anchor.staleness_p99_ms >= $med * 0.9)
                and (.anchor.staleness_p99_ms <= $med * 1.1)) end)]' \
    > "$WORKDIR/campaign-summary.json"
  echo "aggregate: $WORKDIR/campaign-summary.json"
  jq -r '"campaign \(.campaign) commit \(.commit) anchor_median=\(.anchor_fleet_median_ms // "-")ms",
    "hosts: \(.hosts | map("\(.ip) doctor=\(.doctor_ok) calibration=\(.calibration_passed) anchor=\(if .anchor_ok == null then "-" else .anchor_ok end)") | join(", "))",
    "jobs: \(.jobs | length) done / holes: \(.holes | length)",
    (.jobs[] | "  \(.job) @\(.host) attempts=\(.attempts) cells=\(.cells | length)"),
    (.jobs[].cells[] | "    \(.transport)/\(.workload)/\(.regime): capacity=\(.capacity) censored=\(.censored // false)"),
    (.jobs[] | select(.ramp != null) | "    run \(.job): \(.ramp.transport) outcome=\(.ramp.outcome) ramp_score=\(.ramp.score_conns // "-") censored=\(.ramp.censored // "-") cause=\(.ramp.cause // "-")")' \
    "$WORKDIR/campaign-summary.json"
  ;;

cleanup)
  [ -n "$CAMPAIGN" ] || { echo "-campaign required" >&2; exit 2; }
  IDS=$(aws2 ec2 describe-instances \
    --filters "Name=tag:Project,Values=$PROJECT" "Name=tag:Campaign,Values=$CAMPAIGN" \
      "Name=instance-state-name,Values=pending,running,stopping,stopped" \
    | jq -r '[.Reservations[].Instances[].InstanceId] | join(" ")')
  if [ -n "$IDS" ]; then
    # shellcheck disable=SC2086
    aws2 ec2 terminate-instances --instance-ids $IDS > /dev/null
    echo "terminated: $IDS"
    # shellcheck disable=SC2086
    aws2 ec2 wait instance-terminated --instance-ids $IDS
  fi
  SG=$(aws2 ec2 describe-security-groups \
    --filters "Name=tag:Project,Values=$PROJECT" "Name=tag:Campaign,Values=$CAMPAIGN" \
    | jq -r '.SecurityGroups[].GroupId')
  [ -n "$SG" ] && aws2 ec2 delete-security-group --group-id "$SG" > /dev/null && echo "sg deleted: $SG"
  aws2 ec2 delete-key-pair --key-name "$PROJECT-$CAMPAIGN" > /dev/null && echo "key pair deleted"
  ;;

*)
  echo "usage: $0 launch|status|dispatch|aggregate|cleanup ..." >&2; exit 2 ;;
esac
