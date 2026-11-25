#!/usr/bin/env bash
# Builds bridge/ (codec-info translation + payload packing) with the upstream encoder and runs its tests.
# Works on a PC and in Termux (pkg install clang cmake git).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/fetch-upstream.sh" "$ROOT/third_party/samsung-ssc-encoder"

BUILD="$ROOT/out/build-bridge"
cmake -S "$ROOT/bridge" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --config Release
"$BUILD/test_bridge" "$ROOT/third_party/samsung-ssc-encoder/tests/vectors"
