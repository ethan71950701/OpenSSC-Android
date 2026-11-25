#!/usr/bin/env bash
# Builds the upstream encoder on this machine and runs its full conformance suite.
# Needs: git, cmake, a C compiler, python3.   Output: out/conformance-report.md
# Usage: scripts/verify-encoder.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/samsung-ssc-encoder"
BUILD="$ROOT/out/build-host"

"$ROOT/scripts/fetch-upstream.sh" "$SRC"

cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --config Release

mkdir -p "$ROOT/out"
# -I: do not put the (third-party) test directory on sys.path.
python3 -I "$SRC/tests/run_tests.py" \
  --bin "$BUILD/ssc_conformance" \
  --markdown "$ROOT/out/conformance-report.md" | tee "$ROOT/out/conformance-stdout.txt" | tail -n 3

if grep -qE '[1-9][0-9]* failed|, [1-9][0-9]* failed runs' "$ROOT/out/conformance-stdout.txt"; then
  echo "FAIL: see out/conformance-stdout.txt" >&2
  exit 1
fi
echo "PASS: report in out/conformance-report.md"
