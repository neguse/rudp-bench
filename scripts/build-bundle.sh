#!/usr/bin/env bash
# fleet 配布用 self-contained bundle を作る(ADR-0005 §5)。
# 中身は「git archive したソースツリー + 正規パスに重ねた Release 成果物」。
# fleet ホストは展開してそのまま orchestrator config のリポ相対パスで実行できる。
#
# usage: scripts/build-bundle.sh [-o <out.tar.zst>] [--skip-build] [--allow-dirty]
#   BUNDLE_NATIVE  : ビルドする native transport(既定: raw_udp enet kcp gns msquic)
#   BUNDLE_DOTNET  : ビルドする .NET solution(既定: litenetlib websocket magiconion)
set -euo pipefail

cd "$(dirname "$0")/.."

OUT=""
SKIP_BUILD=0
ALLOW_DIRTY=0
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

NATIVE=${BUNDLE_NATIVE:-"raw_udp enet kcp gns msquic"}
DOTNET=${BUNDLE_DOTNET:-"litenetlib websocket magiconion"}

COMMIT=$(git rev-parse HEAD)
SHORT=$(git rev-parse --short HEAD)
if [ -n "$(git status --porcelain)" ]; then
  if [ "$ALLOW_DIRTY" = 1 ]; then
    echo "WARN: dirty tree(reference 用 bundle には使わないこと)" >&2
    SHORT="${SHORT}-dirty"
  else
    echo "ERROR: dirty tree。reference bundle は clean tree 必須(--allow-dirty で検証用のみ許可)" >&2
    exit 1
  fi
fi
OUT=${OUT:-"bundle-${SHORT}.tar.zst"}

case "$(uname -m)" in
  aarch64) RID=linux-arm64 ;;
  x86_64) RID=linux-x64 ;;
  *) echo "unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac

# --- build -------------------------------------------------------------
declare -A SRCDIR=([raw_udp]=servers/raw_udp [enet]=servers/enet [kcp]=servers/kcp
  [gns]=servers/gns [msquic]=servers/msquic)
# config が参照する build dir 名(raw_udp のみ歴史的に underscore なし)
declare -A BUILDDIR=([raw_udp]=build-v2-rawudp [enet]=build-v2-enet [kcp]=build-v2-kcp
  [gns]=build-v2-gns [msquic]=build-v2-msquic)
declare -A SLN=([litenetlib]=LiteNetLibBench [websocket]=WebSocketBench [magiconion]=MagicOnionBench)

if [ "$SKIP_BUILD" = 0 ]; then
  go build -o build-v2/orchestrator ./orchestrator/cmd/orchestrator
  for t in $NATIVE; do
    cmake -S "${SRCDIR[$t]}" -B "${BUILDDIR[$t]}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILDDIR[$t]}" -j"$(nproc)"
  done
  for s in $DOTNET; do
    for role in Server Client; do
      proj="servers/$s/${SLN[$s]}.$role/${SLN[$s]}.$role.csproj"
      dotnet publish -c Release -r "$RID" --self-contained true \
        -o "build-v2-dotnet/${SLN[$s]}.$role" "$proj"
    done
  done
fi

# --- stage -------------------------------------------------------------
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# ソースツリー(tracked file のみ。submodule はビルド時依存なので空で良い)
git archive HEAD | tar -x -C "$STAGE"

# Release 成果物を config が参照する正規パスへ重ねる
install -D -m755 build-v2/orchestrator "$STAGE/build-v2/orchestrator"
for t in $NATIVE; do
  mkdir -p "$STAGE/${BUILDDIR[$t]}"
  # 各 build dir 直下の実行ファイルのみ(benchkit-build のテスト群は runtime 不要)
  find "${BUILDDIR[$t]}" -maxdepth 1 -type f -executable \
    -exec install -m755 -t "$STAGE/${BUILDDIR[$t]}" {} +
done
for s in $DOTNET; do
  for role in Server Client; do
    dest="$STAGE/servers/$s/${SLN[$s]}.$role/bin/Release/net10.0"
    mkdir -p "$dest"
    cp -a "build-v2-dotnet/${SLN[$s]}.$role/." "$dest/"
  done
done

# --- manifest ----------------------------------------------------------
{
  echo "{"
  echo "  \"commit\": \"$COMMIT\","
  echo "  \"rid\": \"$RID\","
  echo "  \"built_on\": \"$( (. /etc/os-release 2>/dev/null; echo "${ID:-unknown}-${VERSION_ID:-rolling}") )\","
  echo "  \"binaries\": ["
  (cd "$STAGE" && find build-v2* servers -type f -executable | sort | while read -r f; do
    echo "    {\"path\": \"$f\", \"sha256\": \"$(sha256sum "$f" | cut -d' ' -f1)\"},"
  done) | sed '$ s/,$//'
  echo "  ]"
  echo "}"
} > "$STAGE/bundle-manifest.json"

tar --zstd -cf "$OUT" -C "$STAGE" .
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)) commit=$SHORT rid=$RID"
