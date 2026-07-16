#!/usr/bin/env bash
# fleet campaign coordinator の骨組み(ADR-0005 enabler #3)。
# ローカルマシンで実行し、AWS 側は campaign-scoped な ephemeral リソースのみ
# (IaC なし。安全網は tag: Project/Campaign → cleanup が全削除)。
# ホストは credential を持たず、配布・回収は SSH/scp のみ。
#
# usage:
#   scripts/fleet/campaign.sh launch  -n <hosts> [-campaign <id>] [-commit <sha7>]
#   scripts/fleet/campaign.sh status  -campaign <id>
#   scripts/fleet/campaign.sh cleanup -campaign <id>
#
# TODO(骨組みの先): cell queue 配布 / 逐次 scp 回収 / interruption handler /
# 集約 + PR 作成。A/A 準備の初回起動で launch/status/cleanup を検証してから積む。
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
HOSTS=1; CAMPAIGN=""; COMMIT=""
while [ $# -gt 0 ]; do
  case "$1" in
    -n) HOSTS="$2"; shift 2 ;;
    -campaign) CAMPAIGN="$2"; shift 2 ;;
    -commit) COMMIT="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

WORKDIR_ROOT="results-v2/fleet"

case "$CMD" in
launch)
  CAMPAIGN=${CAMPAIGN:-"c$(date +%Y%m%d-%H%M%S)"}
  COMMIT=${COMMIT:-$(git rev-parse --short=7 HEAD)}
  WORKDIR="$WORKDIR_ROOT/$CAMPAIGN"
  mkdir -p "$WORKDIR"
  BUNDLE_URL="https://github.com/neguse/rudp-bench/releases/download/$RELEASE_TAG/bundle-$COMMIT.tar.zst"
  # bundle の実在を先に確認(fleet を建ててから 404 に気づかない)
  curl -fsIL -o /dev/null "$BUNDLE_URL" || { echo "ERROR: bundle が無い: $BUNDLE_URL" >&2; exit 1; }

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
  # boot gate の READY は各ホストへ SSH して確認する(後続で自動化)
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
  [ -n "$SG" ] && aws2 ec2 delete-security-group --group-id "$SG" && echo "sg deleted: $SG"
  aws2 ec2 delete-key-pair --key-name "$PROJECT-$CAMPAIGN" > /dev/null && echo "key pair deleted"
  ;;

*)
  echo "usage: $0 launch|status|cleanup ..." >&2; exit 2 ;;
esac
