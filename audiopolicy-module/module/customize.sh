[ "$ARCH" = "arm64" ] || abort "! arm64 only"
ui_print "- OpenSSC Audio Policy 0.2.2: full-software A2DP (needs persist.bluetooth.a2dp_offload.disabled=true)"
ui_print "- Rescue: touch /data/adb/modules/openssc_audiopolicy/disable and reboot"
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
