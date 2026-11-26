[ "$ARCH" = "arm64" ] || abort "! arm64 only"
ui_print "- OpenSSC Probe 0.2.1: writes a report and installs log-only hooks (no data is changed)"
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
