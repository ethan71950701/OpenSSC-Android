[ "$ARCH" = "arm64" ] || abort "! arm64 only"
ui_print "- OpenSSC Probe: no hooks, writes a report only"
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
