#!/system/bin/sh
# Module removed: give A2DP back to the DSP on the next boot.
/data/adb/ksu/bin/resetprop -p persist.bluetooth.a2dp_offload.disabled false
