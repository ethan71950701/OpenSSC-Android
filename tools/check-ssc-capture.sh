#!/bin/sh
# Analyses a btsnoop log for SSC: lists, in time order, every SSC codec element seen on AVDTP
# (capabilities and configurations) and the SSC frame bitrate codes that followed each one.
# Text output only; the log itself is not modified. Needs xxd (Termux: pkg install xxd) and awk.
#
#   sh tools/check-ssc-capture.sh /sdcard/Download/btsnoop.log
#
# How to read the output:
#   ELEMENT 3e   the earbuds' SSC capability (Galaxy Buds 3 Pro reports 0x3E)
#   ELEMENT 0c   a configuration for HiFi 48 kHz  (expected frames: rate f3 = 192 kb/s)
#   ELEMENT 0a   a configuration for UHQ 96 kHz   (expected frames: rate e3 = 584 kb/s)
#   rate xx      SSC frame headers (FF Ex ss xx) counted after that element; codes seen fewer than
#                MIN_COUNT times are hidden because the byte pattern also matches random data.
# Bitrate codes: f0=88k f1=96k f2=128k f3=192k f4=229k f5=328k f6=256k
#                e0=291k e1=308k e2=442k e3=584k e4=886k e5=250k e6=152k
set -eu

LOG="${1:?usage: check-ssc-capture.sh <btsnoop log>}"
MIN_COUNT="${MIN_COUNT:-20}"
[ -r "$LOG" ] || { echo "cannot read $LOG" >&2; exit 1; }
command -v xxd >/dev/null 2>&1 || { echo "xxd not found (Termux: pkg install xxd)" >&2; exit 1; }

# Media codec capability: 07 09 | 00 ff | 75 00 00 00 | 03 01 | <byte>
xxd -p "$LOG" | tr -d '\n' |
  grep -oE '070900ff750000000301[0-9a-f]{2}|ffe[0-9a-f]{3}(e[0-6]|f[0-6])' |
  awk -v min="$MIN_COUNT" '
    function flush(   k) { for (k in c) if (c[k] >= min) printf "    rate %s x%d\n", k, c[k]; split("", c) }
    /^0709/ { flush(); el = substr($0, 21, 2); n_el++; printf "ELEMENT %s\n", el; next }
    { c[substr($0, 7, 2)]++ }
    END { flush(); if (!n_el) print "no SSC codec element found (was the connection set up while logging?)" }'
