[ "$ARCH" = "arm64" ] || abort "! arm64 only"
ui_print "- OpenSSC Audio Policy 0.3: full-software A2DP (turns A2DP offload off while installed)"
ui_print "- Rescue: remove the module in KernelSU (offload is restored), or see README"
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
