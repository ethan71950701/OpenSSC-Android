#!/system/bin/sh
# Collects a text-only report about a phone's Bluetooth stack. Run it inside Termux on the phone
# (grant Termux root in KernelSU first). It copies no binaries: only names, versions, hashes and strings.
#
#   sh tools/probe-device.sh            # writes /sdcard/Download/ssc-probe-<model>-<timestamp>.txt
#
# Run it on BOTH phones (Galaxy S25+ and Xiaomi 13), with the Galaxy Buds connected and playing, then pull
# the two reports to the PC. Bluetooth MAC addresses are masked as XX:XX:XX:XX:XX:XX at the end, but
# review the report before sharing it anyway (property dumps can contain other device-specific values).

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
  echo "ssc-probe report (v2)"
  echo "date: $(date)"
  echo "have_su: $HAVE_SU"

  section "root check"
  R 'id'

  section "device / build"
  for p in ro.product.manufacturer ro.product.model ro.product.device ro.board.platform ro.soc.model \
           ro.product.cpu.abi ro.build.version.release ro.build.version.sdk ro.build.version.security_patch \
           ro.build.version.incremental ro.build.display.id ro.build.version.oneui \
           ro.mi.os.version.name ro.mi.os.version.code ro.miui.ui.version.name \
           ro.bluetooth.library_name ro.bluetooth.a2dp_offload.supported persist.bluetooth.a2dp_offload.disabled; do
    printf '%s=%s\n' "$p" "$(getprop $p)"
  done
  echo "kernel: $(uname -a)"

  section "bluetooth APEX"
  R 'ls -d /apex/com.android.bt* /apex/com.android.btservices* 2>/dev/null'
  R 'pm list packages --apex-only --show-versioncode 2>/dev/null | grep -iE "bluetooth|\.bt"'
  R 'ls -la /apex/com.android.bt/lib64/ 2>/dev/null'

  section "all bluetooth libraries on the device"
  R 'find /system /system_ext /vendor /product /odm /apex/com.android.bt -type f \( -name "libbluetooth*" -o -name "libbt-*" -o -name "libbt_*" -o -name "lib_bt_*" -o -name "*a2dp*.so" \) 2>/dev/null | head -n 120'

  section "codec-related libraries anywhere in the system"
  R 'find /system /system_ext /vendor /product /odm /apex -type f \( -iname "*scalable*" -o -iname "*ldac*" -o -iname "*aptx*" -o -iname "*lhdc*" -o -iname "*liblc3*" -o -iname "*mihc*" \) 2>/dev/null | head -n 80'

  section "sha256 of Samsung encoder library, if present"
  R 'for f in $(find /system /system_ext /vendor /apex -type f -iname "libScalable_Encoder.so" 2>/dev/null); do sha256sum "$f"; ls -l "$f"; done'

  # Strings in every main Bluetooth stack library found above (versioned /apex/...@N mounts are skipped).
  BTLIBS="$(R 'find /system /system_ext /vendor /product /odm /apex/com.android.bt -type f \( -name "libbluetooth.so" -o -name "libbluetooth_jni.so" -o -name "libbluetooth_qti.so" -o -name "libbluetooth_qti_*.so" \) 2>/dev/null' | grep -v '@' | head -n 8)"
  for f in $BTLIBS; do
    section "library: $f"
    R "ls -l '$f'; sha256sum '$f'"
    section "strings in $f: A2DP vendor codec framework"
    R "grep -a -o -E '[[:print:]]*(A2DP_Vendor|a2dp_vendor_|A2DP_Build|A2DP_Parse|a2dp_codec_config)[[:print:]]*' '$f' | sort -u | head -n 250"
    section "strings in $f: codec names (ssc / ldac / lhdc / aptx / mihc / opus / lc3)"
    R "grep -a -o -E '[[:print:]]*([Ss][Ss][Cc]|[Ll][Dd][Aa][Cc]|[Ll][Hh][Dd][Cc]|[Aa]ptx|MIHC|[Mm]ihc|[Oo]pus|LC3)[[:print:]]*' '$f' | sort -u | head -n 250"
  done

  section "bluetooth services / HALs"
  R 'service list 2>/dev/null | grep -iE "bluetooth|a2dp"'
  R 'ls /vendor/etc/vintf/manifest 2>/dev/null | grep -i bluetooth'

  section "bluetooth props"
  R 'getprop | grep -iE "bluetooth|bt\.|a2dp"'

  section "dumpsys bluetooth_manager (codec lines only)"
  R 'dumpsys bluetooth_manager 2>/dev/null | grep -iE "codec|a2dp|sample ?rate|bits ?per|channel ?mode|bitrate|offload" | head -n 150'

  section "HCI snoop log locations"
  R 'find /data/misc/bluetooth /data/log -iname "*snoop*" 2>/dev/null | head -n 20'
  echo "btsnooplogmode=$(R 'getprop persist.bluetooth.btsnooplogmode')"
} > "$OUT" 2>&1

# Mask Bluetooth MAC addresses (xx:xx:xx:xx:xx:xx) everywhere in the report.
if sed -i -E 's/([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/XX:XX:XX:XX:XX:XX/g' "$OUT" 2>/dev/null; then
  echo "MAC addresses masked."
else
  echo "WARNING: could not mask MAC addresses; review the report before sharing." >&2
fi

echo "report written: $OUT"
echo "lines: $(wc -l < "$OUT")"
echo "pull it with:  adb pull \"$OUT\""
