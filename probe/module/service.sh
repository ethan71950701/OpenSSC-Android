#!/system/bin/sh
MODDIR=${0%/*}
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 5; done
echo 0 > "$MODDIR/boot_count"
