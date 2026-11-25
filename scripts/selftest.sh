#!/usr/bin/env bash
# Encodes the upstream test vectors with ssc_enc_tool and checks that every output frame has the
# expected size for its bitrate (sizes measured by the upstream conformance suite).
# Usage: scripts/selftest.sh <ssc_enc_tool> <vectors_dir>
set -euo pipefail

TOOL="${1:?path to ssc_enc_tool}"
VEC="${2:?path to tests/vectors}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0

# check <label> <rate> <format> <input> <bitrate> <expected_frame_bytes>
check() {
  local label="$1" rate="$2" fmt="$3" in="$4" br="$5" want="$6" out="$TMP/o.ssc"
  "$TOOL" -q -r "$rate" -f "$fmt" -b "$br" "$VEC/$in" "$out"
  local size first
  size="$(wc -c < "$out")"
  # file = repeated [u16 LE length][frame]; the first length must equal the expected frame size
  first="$(od -An -tu2 -N2 -v "$out" | tr -d ' ')"
  if [ "$first" = "$want" ] && [ $((size % (want + 2))) -eq 0 ] && [ "$size" -gt 0 ]; then
    printf 'ok    %-22s %7s bps  frame=%4s B  frames=%s\n' "$label" "$br" "$want" $((size / (want + 2)))
  else
    printf 'FAIL  %-22s %7s bps  want=%s got_first=%s size=%s\n' "$label" "$br" "$want" "$first" "$size"
    fail=1
  fi
}

# 44.1/48 kHz mode, 16-bit input (frame bytes from the conformance report)
for pair in 88000:224 96000:244 128000:324 152000:376 192000:484 229000:576 250000:616 256000:630 \
            291000:716 308000:758 328000:806 442000:1086 584000:1434 886000:2172; do
  check "44.1k s16" 44100 s16 mix.s16le "${pair%%:*}" "${pair##*:}"
done
# 96 kHz UHQ mode, 16-bit samples fed as 24-bit
for pair in 152000:174 291000:330 584000:660 886000:1000; do
  check "96k s16as24" 96000 s16as24 mix96k.s16le "${pair%%:*}" "${pair##*:}"
done

if [ "$fail" -ne 0 ]; then
  echo "SELFTEST FAILED" >&2
  exit 1
fi
echo "SELFTEST PASSED"
