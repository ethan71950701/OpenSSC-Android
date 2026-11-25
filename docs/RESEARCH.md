# 研究紀錄

每一條都標示來源與驗證狀態。**已驗證** = 我們在自己的裝置或機器上親眼看到；**第三方** = 來自網頁或論壇，未獨立驗證。

## 1. 在 Galaxy S25+ 上實測到的事實（已驗證）

裝置：One UI 8.5 / Android 16，build `BP4A.251205.006.S9360ZHSCCZG1`，安全更新 2026-07-05，核心 `6.6.98-android15-8`，KernelSU（臨時 root）。

- 蓝牙模組 APEX 名稱是 **`com.android.bt`**（不是 `com.android.btservices`），另有版本化掛載 `com.android.bt@361099999`。
- `libScalable_Encoder.so` 共三個路徑，**內容完全相同**（SHA-256 `90c4c1cbd59b0edb9df6d255acc1f752e542e0cb99d3de8e842afc7ca9914ce7`）：
  `/system/lib64/`、`/apex/com.android.bt/lib64/`（符號連結）、`/apex/com.android.bt@361099999/lib64/`。
- 這個 `.so` 沒有被 strip，`nm -D` 導出：
  `ssc_encoder_create`、`ssc_encoder_destroy`、`ssc_encoder_get_size`、`ssc_encoder_init`、`ssc_encode`，以及一整組 CELT 風格的內部函數
  （`ssc_ec_enc_*`、`ssc_mdct_forward`、`ssc_pitch_search`、`ssc_haar1`、`ssc_comb_filter`、`ssc_compute_band_energies`、`ssc_quant_*` 等）。
- `libbluetooth_jni.so`（14,427,584 位元組）內有三星的 SSC 整合字串：
  - 原始碼路徑 `packages/modules/Bluetooth/system/stack/a2dp/a2dp_vendor_ssc.cc`
  - 函數：`A2DP_BuildInfoSsc`、`A2DP_ParseInfoSsc`、`A2DP_IsVendorSourceCodecValidSsc`、`A2DP_IsVendorPeerSinkCodecValidSsc`、
    `A2DP_VendorAdjustCodecSsc`、`A2DP_VendorCodecTypeEqualsSsc`、`A2DP_VendorGetBitRateSsc`、`A2DP_VendorGetEncoderInterfaceSsc`、
    `A2DP_VendorGetTrackBitrateLimitationSsc`、`A2DP_VendorGetTrackBitsPerSampleSsc`、`A2DP_VendorInitCodecConfigSsc`、`A2DP_VendorUnloadEncoderSsc`、
    `a2dp_ssc_encode_frames`、`a2dp_ssc_get_num_frame`、`a2dp_vendor_ssc_encoder_cleanup`、`a2dp_vendor_ssc_feeding_flush`
  - 編碼器經 `dlsym` 載入（字串 `ssc_encoder_create_func ({})`）
  - 另有三星 AIDL vendor HAL：`aidl::vendor::samsung::hardware::bluetooth::audio::SehCodecCapabilities` / `SehCodecConfiguration`
    （`sscCapabilities`、`sscConfig`），以及 `ssc_offloading_capability_match`，即硬體卸載路徑
  - 功能開關字串：`A2DPSM-SSC HiFi 1st is disabled`、`btif_av_need_dual_play_for_ssc`
- 結論：在三星的棧裡，SSC 是**標準的 A2DP vendor codec**（與 AOSP 的 LDAC / aptX 同型態），不是特殊握手。

## 2. 開源 SSC 編碼器（第三方，我們已在本機驗證可用）

- 倉庫：https://github.com/WillyBilly06/samsung-ssc-encoder ，固定使用 commit `94f1896f1b80830554b0cc8deec47ab04130e19f`，BSD-3-Clause（部分源自 Opus 1.1 CELT，保留 Xiph.Org 版權聲明）。
- 解碼器（ESP32 喇叭韌體，非耳機端）：https://github.com/WillyBilly06/samsung-ssc-decoder
- **已驗證（我們自己跑的）**：編譯成功；上游 `tests/run_tests.py` 137 次執行 0 失敗；`scripts/selftest.sh` 18 項幀大小檢查全過。
- 介面：C99，每幀 864 個交錯立體聲採樣；`ssc_encoder_create(rate)`（44100/48000/96000）、`ssc_encode`（int16）、`ssc_encode_s32`（24-bit）、`ssc_encode_float`。
  **注意：這和三星 `.so` 的導出介面不同**（三星是 `get_size` + `init` 的形式），開源版不是三星二進位檔的直接替代，但**位元流**對齊。
- A2DP 識別：vendor `0x00000075`、codec `0x0103`；Seamless 與 UHQ 沒有獨立 codec ID，靠封包內的模式旗標區分。
- 14 個碼率：88、96、128、152、192、229、250、256、291、308、328、442、584、886 kbps。
- 各碼率的單幀位元組數（44.1/48 kHz 模式）：
  88k=224、96k=244、128k=324、152k=376、192k=484、229k=576、250k=616、256k=630、291k=716、308k=758、328k=806、442k=1086、584k=1434、886k=2172。
  96 kHz UHQ 模式：152k=174、291k=330、584k=660、886k=1000。
- **未驗證**：與三星比對用的 `.ssc` 參考幀是作者提供的（來自作者的三星手機擷取），我們沒有獨立核對來源。
- **未驗證**：作者明說**尚未在真實 Galaxy Buds 上測試**。

## 3. 論壇說法（第三方，未驗證）

來自使用者提供的論壇截圖（作者在小米 17 Pro Max 上；回覆中另有一人聲稱也成功）：

- 小米上開發者選項顯示「正在串流：SSC，96 kHz，24 bit」，藍牙頁面顯示 Galaxy Buds3 Pro 旁有 SSC 標籤。
- 軟體編碼，不經 DSP；小米的音效由高通 DSP 處理，走 SSC 軟體鏈路時音效被繞過。
- 三星只開放到 584 kbps；作者聲稱強制 886 kbps 時封包 MTU 達 1012，超過軟體設定的 1005 上限。（**未驗證**）
- 48 kHz SSC 碼率表為 192/192/128/88，並稱內部會 48→44.1 重採樣。（**未驗證**）
- **方法未公開**：截圖看不出是系統修改、root 還是模組，也看不出 SoC 與蓝牙栈版本。小米 17 Pro Max 與小米 13（骁龙 8 Gen 2）是不同平台。

## 4. 其他參考

- 三星對 SSC 的官方說明：https://www.samsung.com/in/support/mobile-devices/what-is-scalable-codec/
- 以 LSPosed 讓三星應用在非 One UI 上運行（若之後發現 Galaxy Wearable 有裝置檢查可參考）：https://github.com/Simon1511/WearableSpoof
- LSPosed 原專案已久未更新，社群主要用 JingMatrix 的 Vector 分支；有報告指 HyperOS 上 LSPosed 常顯示啟用但模組不生效（第三方，未驗證）。
- 搜尋範圍內**沒有找到**專門在小米 HyperOS 上啟用 SSC 的現成開源模組。

## 5. 仍然未知（決定整合方案的關鍵）

| 未知 | 怎麼查 | 影響 |
|---|---|---|
| 小米 13 的 HyperOS / Android 版本，蓝牙 APEX 名稱與版本 | `tools/probe-device.sh` | 決定要對哪一版 AOSP 蓝牙原始碼 |
| 小米 13 的 `libbluetooth.so` 有沒有 `A2DP_Vendor*` 框架（LDAC / LHDC 等）、有沒有被 strip | 同上 | 決定走「改寫既有 vendor codec」還是「整個新增」 |
| Buds 3 Pro 在 Get Capabilities 回報的 SSC 能力位元組長什麼樣 | S25+ 上的 btsnoop | A2DP codec info element 的精確格式 |
| Buds 在非三星來源下是否仍會選 SSC | 實測 | 若不會，要另找原因（可能有額外協商） |
| APEX 唯讀的繞過方式在小米 13 + KernelSU 上哪個可行 | 實測 | 決定部署方式 |
