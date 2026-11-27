[ "$ARCH" = "arm64" ] || abort "! arm64 only"
ui_print "- OpenSSC Probe 0.5.7: SSC audio in translate mode (enable_translate); ssc_mute keeps it silent"
# Keep the user's switches across updates (KernelSU replaces the module folder).
OLD=/data/adb/modules/openssc_probe
for f in enable_translate diag_targets.txt ssc_mute ssc_ts_step ssc_fpp ssc_interval_ms ssc_uhq ssc_kbps ssc_pcm_hz; do
  if [ -f "$OLD/$f" ]; then cp -f "$OLD/$f" "$MODPATH/$f" && ui_print "- kept $f"; fi
done
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
