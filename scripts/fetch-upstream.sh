#!/usr/bin/env bash
# Fetches the open-source SSC encoder at a pinned commit into third_party/ (git-ignored).
# Usage: scripts/fetch-upstream.sh [dest_dir]
set -euo pipefail

UPSTREAM_URL="https://github.com/WillyBilly06/samsung-ssc-encoder"
UPSTREAM_COMMIT="94f1896f1b80830554b0cc8deec47ab04130e19f"   # BSD-3-Clause; see docs/RESEARCH.md

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$ROOT/third_party/samsung-ssc-encoder}"

if [ ! -d "$DEST/.git" ]; then
  mkdir -p "$(dirname "$DEST")"
  git clone "$UPSTREAM_URL" "$DEST"
fi

git -C "$DEST" fetch --quiet origin || true
git -C "$DEST" checkout --quiet "$UPSTREAM_COMMIT"

got="$(git -C "$DEST" rev-parse HEAD)"
if [ "$got" != "$UPSTREAM_COMMIT" ]; then
  echo "commit mismatch: expected $UPSTREAM_COMMIT, got $got" >&2
  exit 1
fi
echo "OK: $DEST @ ${got:0:12}"
