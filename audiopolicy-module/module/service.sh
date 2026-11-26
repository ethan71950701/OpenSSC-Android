#!/system/bin/sh
MODDIR=${0%/*}
LOG="$MODDIR/status.txt"
RP=/data/adb/ksu/bin/resetprop
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 5; done
echo 0 > "$MODDIR/boot_count"
T=/odm/etc/audio/sku_kalama_qssi/audio_policy_configuration.xml
c=$(grep -c 'AUDIO_FORMAT_OPUS' "$T" 2>/dev/null)
echo "boot completed $(date); OPUS entries (expect 3) visible in active policy: ${c:-0}; runtime offload.disabled=$(getprop persist.bluetooth.a2dp_offload.disabled)" >> "$LOG"
# Watch the stored value for 10 minutes and put it back if something resets it (logged, to find the culprit).
[ -x "$RP" ] && [ ! -f "$MODDIR/no_prop" ] && [ "${c:-0}" = "3" ] || exit 0
i=0
while [ $i -lt 20 ]; do
  sleep 30; i=$((i + 1))
  v=$("$RP" -p persist.bluetooth.a2dp_offload.disabled 2>/dev/null)
  if [ "$v" != "true" ]; then
    echo "$(date): stored value was '$v' (runtime '$(getprop persist.bluetooth.a2dp_offload.disabled)'), set back to true" >> "$LOG"
    "$RP" -p persist.bluetooth.a2dp_offload.disabled true
  fi
done
