#!/system/bin/sh
# Boot counter: if boot does not complete, the third attempt turns the module off.
MODDIR=${0%/*}
f="$MODDIR/boot_count"
n="$(cat "$f" 2>/dev/null)"
case "$n" in ''|*[!0-9]*) n=0 ;; esac
n=$((n + 1))
if [ "$n" -ge 3 ]; then
  touch "$MODDIR/disable_probe" "$MODDIR/disable"
  n=0
fi
echo "$n" > "$f"
sync
