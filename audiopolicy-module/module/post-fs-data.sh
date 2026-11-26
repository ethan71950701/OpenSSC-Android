#!/system/bin/sh
# Full-software A2DP: generates a patched copy of the active audio policy and bind-mounts it over the original.
# Nothing is written to /odm or /vendor; removing the module and rebooting restores the stock file.
MODDIR=${0%/*}
LOG="$MODDIR/status.txt"
TARGET="${OPENSSC_POLICY:-/odm/etc/audio/sku_kalama_qssi/audio_policy_configuration.xml}"
PATCHED="$MODDIR/audio_policy_patched.xml"
echo "post-fs-data $(date)" > "$LOG"

# Boot guard: if the previous two boots never reached service.sh, stop mounting.
f="$MODDIR/boot_count"
n="$(cat "$f" 2>/dev/null)"
case "$n" in ''|*[!0-9]*) n=0 ;; esac
n=$((n + 1)); echo "$n" > "$f"; sync
if [ "$n" -ge 3 ]; then
  echo "SKIP: boot guard tripped ($n boots without completion); module disabled" >> "$LOG"
  touch "$MODDIR/disable"; echo 0 > "$f"; exit 0
fi
[ -f "$MODDIR/skip_mount_policy" ] && { echo "SKIP: skip_mount_policy present" >> "$LOG"; exit 0; }
[ -f "$TARGET" ] || { echo "SKIP: $TARGET not found" >> "$LOG"; exit 0; }

# Full-software mode only makes sense when the Bluetooth stack itself has A2DP offload turned off; otherwise
# A2DP would be routed to the software module while the stack waits on the DSP, and nothing would play.
# Persistent properties are loaded after post-fs-data, so read the stored value from the property file.
PP="${OPENSSC_PP:-/data/property/persistent_properties}"
# Prefer KernelSU's resetprop -p (reads the stored value); fall back to scanning the protobuf file
# (name, then 0x12 <len>, then the value).
RP=/data/adb/ksu/bin/resetprop
stored=""
[ -x "$RP" ] && stored=$("$RP" -p persist.bluetooth.a2dp_offload.disabled 2>/dev/null)
if [ -z "$stored" ] && grep -a -q 'persist\.bluetooth\.a2dp_offload\.disabled..true' "$PP" 2>/dev/null; then stored=true; fi
if [ "$stored" != "true" ]; then
  echo "SKIP: stored persist.bluetooth.a2dp_offload.disabled is '$stored', not true; stock policy kept" >> "$LOG"; exit 0
fi

# 1) bluetooth_qti (software) A2DP ports: the only ports carrying AUDIO_FORMAT_FORCE_AOSP. Accept the codecs we use.
# 2) primary (DSP offload) A2DP ports: narrow them to LHDC (never offered by the Buds) so they stop matching.
cnt=$(grep -c 'encodedFormats="AUDIO_FORMAT_FORCE_AOSP"' "$TARGET")
PRI='AUDIO_FORMAT_SBC AUDIO_FORMAT_AAC AUDIO_FORMAT_APTX AUDIO_FORMAT_APTX_HD AUDIO_FORMAT_LDAC AUDIO_FORMAT_CELT AUDIO_FORMAT_APTX_ADAPTIVE AUDIO_FORMAT_APTX_TWSP AUDIO_FORMAT_LC3 AUDIO_FORMAT_LHDC'
pcnt=$(grep -c "encodedFormats=\"$PRI\"" "$TARGET")
if [ "$cnt" != "3" ] || [ "$pcnt" != "3" ]; then
  echo "SKIP: expected 3+3 A2DP ports, found $cnt software / $pcnt primary (different firmware?)" >> "$LOG"; exit 0
fi
sed -e 's/encodedFormats="AUDIO_FORMAT_FORCE_AOSP"/encodedFormats="AUDIO_FORMAT_FORCE_AOSP AUDIO_FORMAT_SBC AUDIO_FORMAT_AAC AUDIO_FORMAT_OPUS"/g' \
    -e "s/encodedFormats=\"$PRI\"/encodedFormats=\"AUDIO_FORMAT_LHDC\"/g" \
  "$TARGET" > "$PATCHED.tmp" || { echo "SKIP: sed failed" >> "$LOG"; exit 0; }
ok=$(grep -c 'AUDIO_FORMAT_FORCE_AOSP AUDIO_FORMAT_SBC AUDIO_FORMAT_AAC AUDIO_FORMAT_OPUS' "$PATCHED.tmp")
pok=$(grep -c 'encodedFormats="AUDIO_FORMAT_LHDC"' "$PATCHED.tmp")
lo=$(wc -l < "$TARGET"); lp=$(wc -l < "$PATCHED.tmp")
if [ "$ok" != "3" ] || [ "$pok" != "3" ] || [ "$lo" != "$lp" ] || ! grep -q '</audioPolicyConfiguration>' "$PATCHED.tmp"; then
  echo "SKIP: patch check failed (ok=$ok pok=$pok lines $lo/$lp)" >> "$LOG"; rm -f "$PATCHED.tmp"; exit 0
fi
mv -f "$PATCHED.tmp" "$PATCHED"
chmod 0644 "$PATCHED"; chown 0:0 "$PATCHED"
ctx=$(ls -Z "$TARGET" | awk '{print $1}')
case "$ctx" in *:*) chcon "$ctx" "$PATCHED" ;; *) echo "warn: no SELinux context read" >> "$LOG" ;; esac
if mount --bind "$PATCHED" "$TARGET"; then
  echo "OK: bind-mounted patched policy over $TARGET (context $ctx)" >> "$LOG"
else
  echo "FAIL: mount --bind returned error" >> "$LOG"
fi
