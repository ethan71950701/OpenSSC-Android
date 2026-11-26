#!/usr/bin/env bash
# Packs the KernelSU module: out/openssc-audiopolicy-<version>.zip
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
V=$(sed -n 's/^version=//p' "$ROOT/module/module.prop")
mkdir -p "$ROOT/out"
(cd "$ROOT/module" && rm -f "../out/openssc-audiopolicy-$V.zip" && zip -q -r -X "../out/openssc-audiopolicy-$V.zip" .)
echo "built out/openssc-audiopolicy-$V.zip"
