#!/usr/bin/env bash
# Builds the command-line encoder tool (ssc_enc_tool) with a plain C compiler, then runs the self-test.
# Works on a PC and inside Termux on a phone (pkg install clang git).
#   CC=gcc scripts/build-tool.sh        # pick another compiler
#   SKIP_SELFTEST=1 scripts/build-tool.sh
# Output: out/<arch>/ssc_enc_tool
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/samsung-ssc-encoder"
CC="${CC:-$(command -v clang || command -v gcc || true)}"
[ -n "$CC" ] || { echo "no C compiler found (Termux: pkg install clang)" >&2; exit 1; }

"$ROOT/scripts/fetch-upstream.sh" "$SRC"

ARCH="$(uname -m)"
OUT="$ROOT/out/$ARCH"
mkdir -p "$OUT"

"$CC" -std=c99 -O2 -Wall -I"$SRC/include" \
  "$SRC"/src/*.c "$SRC/examples/ssc_enc_tool.c" \
  -o "$OUT/ssc_enc_tool" -lm
echo "built: $OUT/ssc_enc_tool ($CC, $ARCH)"

if [ "${SKIP_SELFTEST:-0}" != "1" ]; then
  "$ROOT/scripts/selftest.sh" "$OUT/ssc_enc_tool" "$SRC/tests/vectors"
fi
