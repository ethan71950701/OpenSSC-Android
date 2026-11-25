#!/system/bin/sh
# Collects a text-only report about a phone's Bluetooth stack. Run it inside Termux on the phone
# (grant Termux root in KernelSU first). It copies no binaries: only names, versions, hashes and strings.
#
#   sh tools/probe-device.sh            # writes /sdcard/Download/ssc-probe-<model>-<timestamp>.txt
#
# Run it on BOTH phones (Galaxy S25+ and Xiaomi 13), then pull the two reports to the PC.
# Review the report before sharing: it contains model / build ids but no serial numbers or MAC addresses
# (the dumpsys section is filtered to codec lines).

OUTDIR="${OUTDIR:-/sdcard/Download}"
MODEL="$(getprop ro.product.model | tr -c 'A-Za-z0-9\n' '_')"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$OUTDIR/ssc-probe-$MODEL-$STAMP.txt"
mkdir -p "$OUTDIR" 2>/dev/null

HAVE_SU=1
command -v su >/dev/null 2>&1 || HAVE_SU=0

# run <command string> as root when possible, else as the current user
R() {
  if [ "$HAVE_SU" = 1 ]; then su -c "$1" 2>&1; else sh -c "$1" 2>&1; fi
}
section() { printf '\n===== %s =====\n' "$1"; }

{
  echo "ssc-probe report"
  echo "date: $(date)"
  echo "have_su: $HAVE_SU"

  section "root check"
  R 'id'

  section "device / build"
  for p in ro.product.manufacturer ro.product.model ro.product.device ro.board.platform ro.soc.model \
           ro.product.cpu.abi ro.build.version.release ro.build.version.sdk ro.build.version.security_patch \
           ro.build.version.incremental ro.build.display.id ro.build.version.oneui \
           ro.mi.os.version.name ro.mi.os.version.code ro.miui.ui.version.name; do
    printf '%s=%s\n' "$p" "$(getprop $p)"
  done
  echo "kernel: $(uname -a)"

  section "bluetooth APEX"
  R 'ls -d /apex/com.android.bt* /apex/com.android.btservices* 2>/dev/null'
  R 'pm list packages --apex-only --show-versioncode 2>/dev/null | grep -iE "bluetooth|\.bt"'
  R 'ls -la /apex/com.android.bt/lib64/ 2>/dev/null | grep -iE "blue|bt|scal|ssc|ldac|aptx|lhdc|lc3|opus"'
  R 'ls -la /apex/com.android.bt/lib/ 2>/dev/null | grep -iE "blue|bt|scal|ssc|ldac|aptx|lhdc|lc3|opus"'

  section "codec-related libraries anywhere in the system"
  R 'find /system /system_ext /vendor /product /odm /apex -type f \( -iname "*scalable*" -o -iname "*ssc*" -o -iname "*ldac*" -o -iname "*aptx*" -o -iname "*lhdc*" -o -iname "*liblc3*" \) 2>/dev/null | head -n 80'

  section "sha256 of Samsung encoder library, if present"
  R 'for f in $(find /system /system_ext /vendor /apex -type f -iname "libScalable_Encoder.so" 2>/dev/null); do sha256sum "$f"; ls -l "$f"; done'

  section "strings: A2DP vendor codec functions (libbluetooth.so)"
  R 'f=/apex/com.android.bt/lib64/libbluetooth.so; [ -f $f ] && grep -a -o "[[:print:]]*A2DP_Vendor[[:print:]]*" $f | sort -u | head -n 200'

  section "strings: a2dp_vendor_* sources (libbluetooth.so)"
  R 'f=/apex/com.android.bt/lib64/libbluetooth.so; [ -f $f ] && grep -a -o "[[:print:]]*a2dp_vendor_[[:print:]]*" $f | sort -u | head -n 120'

  section "strings: ssc (libbluetooth.so)"
  R 'f=/apex/com.android.bt/lib64/libbluetooth.so; [ -f $f ] && grep -a -o "[[:print:]]*[Ss][Ss][Cc][[:print:]]*" $f | sort -u | head -n 120'

  section "strings: ssc (libbluetooth_jni.so)"
  R 'f=/apex/com.android.bt/lib64/libbluetooth_jni.so; [ -f $f ] && grep -a -o "[[:print:]]*[Ss][Ss][Cc][[:print:]]*" $f | sort -u | head -n 120'

  section "strings: other vendor codecs (libbluetooth.so)"
  R 'f=/apex/com.android.bt/lib64/libbluetooth.so; [ -f $f ] && grep -a -o "[[:print:]]*\(ldac\|Ldac\|LDAC\|aptx\|Aptx\|lhdc\|Lhdc\|LHDC\)[[:print:]]*" $f | sort -u | head -n 120'

  section "bluetooth services / HALs"
  R 'service list 2>/dev/null | grep -iE "bluetooth|a2dp"'
  R 'ls /vendor/etc/vintf/manifest 2>/dev/null | grep -i bluetooth'

  section "bluetooth props"
  R 'getprop | grep -iE "bluetooth|bt\.|a2dp"'

  section "dumpsys bluetooth_manager (codec lines only)"
  R 'dumpsys bluetooth_manager 2>/dev/null | grep -iE "codec|a2dp|sample ?rate|bits ?per|channel ?mode|bitrate" | head -n 150'

  section "HCI snoop log locations"
  R 'find /data/misc/bluetooth /data/log -iname "*snoop*" 2>/dev/null | head -n 20'
  R 'settings get secure bluetooth_hci_log'
  echo "persist.bluetooth.btsnooplogmode=$(getprop persist.bluetooth.btsnooplogmode)"
} > "$OUT" 2>&1

echo "report written: $OUT"
echo "lines: $(wc -l < "$OUT")"
echo "pull it with:  adb pull \"$OUT\""
