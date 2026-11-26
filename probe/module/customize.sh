[ "$ARCH" = "arm64" ] || abort "! arm64 only"
ui_print "- OpenSSC Probe 0.3.6: log-only hooks; translate mode only if enable_translate exists"
# Keep the user's switches across updates (KernelSU replaces the module folder).
OLD=/data/adb/modules/openssc_probe
for f in enable_translate diag_targets.txt; do
  if [ -f "$OLD/$f" ]; then cp -f "$OLD/$f" "$MODPATH/$f" && ui_print "- kept $f"; fi
done
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
