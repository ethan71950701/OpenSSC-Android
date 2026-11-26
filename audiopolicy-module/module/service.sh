#!/system/bin/sh
MODDIR=${0%/*}
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 5; done
echo 0 > "$MODDIR/boot_count"
T=/odm/etc/audio/sku_kalama_qssi/audio_policy_configuration.xml
c=$(grep -c 'AUDIO_FORMAT_OPUS' "$T" 2>/dev/null)
echo "boot completed $(date); OPUS entries (expect 3) visible in active policy: ${c:-0}" >> "$MODDIR/status.txt"
