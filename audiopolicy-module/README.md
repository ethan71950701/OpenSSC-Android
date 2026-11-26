# OpenSSC Audio Policy（全軟體 A2DP 路徑）

小米 13（HyperOS，kalama）的 A2DP 預設走高通 DSP 硬體 offload，PCM 不會經過藍牙程序，所以藍牙棧裡的編碼器（Opus 槽，之後換成 SSC）永遠拿不到音訊。這個 KernelSU 模組配合藍牙 offload 關閉，把 A2DP 全部導到軟體路徑。

## 它做什麼

開機時（post-fs-data）在手機上複製作用中的音訊設定
`/odm/etc/audio/sku_kalama_qssi/audio_policy_configuration.xml`，只改 6 行，再用 bind mount 蓋在原檔上：

- `bluetooth_qti`（軟體模組）的 3 個 A2DP 輸出：`AUDIO_FORMAT_FORCE_AOSP` 之外再接受 SBC、AAC、OPUS。
- `primary`（DSP offload）的 3 個 A2DP 輸出：只留 LHDC（Buds 不會用），讓它們不再搶走 A2DP。

模組不附帶任何小米檔案，也不寫入 /odm 或 /vendor。

## 安全條件

- 只有在**已儲存的** `persist.bluetooth.a2dp_offload.disabled` 是 `true` 時才套用（用 KernelSU 的 `resetprop -p` 讀；這支手機上一般 `setprop` 不會存起來）。否則保持原廠設定，避免「設定走軟體、藍牙走 DSP」造成完全沒聲音。
- 找不到剛好 3+3 個目標行（韌體不同）就不套用。
- 連續兩次開機沒完成就自動停用。
- 結果寫在模組資料夾的 `status.txt`。

## 開啟 / 救回

```
su -c '/data/adb/ksu/bin/resetprop -p persist.bluetooth.a2dp_offload.disabled true'
```

救回（兩行都做，再重開機）：

```
su -c '/data/adb/ksu/bin/resetprop -p persist.bluetooth.a2dp_offload.disabled false'
```

```
su -c 'touch /data/adb/modules/openssc_audiopolicy/disable'
```

注意：Java 層的 `dumpsys` 仍會顯示 `mA2dpOffloadEnabled: true`，但原生層 `is_hal_offloading` 實測回傳 0，以原生層為準。
