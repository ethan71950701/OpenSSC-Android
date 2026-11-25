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

## 1b. 在小米 13 上實測到的事實（已驗證，來自探測報告與 btsnoop）

裝置：2211133C（fuxi），SM8550（kalama），**Android 17 / SDK 37**，HyperOS `OS4.0.31.0.XEOCNXM`，安全更新 2026-09-05，核心 `5.15.194-android13-8`，KernelSU。

**蓝牙棧**
- 蓝牙 APEX：`com.android.bt`，versionCode **370399999**（S25+ 是 361099999）。
- APEX 的 `lib64/` 裡**沒有 `libbluetooth.so`**，只有 `libbluetooth_jni.so`（16,266,672 位元組）、Rust 庫與高通 HAL 庫。
  S25+ 的 A2DP 程式碼也在 `libbluetooth_jni.so` 裡，所以 v1 探測腳本去搜 `libbluetooth.so` 是我寫錯了目標（已在 v2 修正）。
- `ro.bluetooth.library_name = libbluetooth_qti.so`，但**整個系統找不到這個檔案**（`/system`、`/system_ext`、`/vendor`、`/product`、`/odm` 都沒有），
  只找到 `/vendor/lib*/libbluetooth_audio_session*.so`（音訊 HAL 的 session 庫）。這個屬性看起來是遺留設定。
- **實際的蓝牙棧是 `/apex/com.android.bt/lib64/libbluetooth_jni.so`**，由小米的分支 **`packages/modules/MiuiBluetooth`** 編譯。
  其中的 A2DP vendor codec 原始檔（來自字串）：
  `a2dp_vendor_aptx(.cc/_encoder.cc)`、`a2dp_vendor_aptx_hd(.cc/_encoder.cc)`、`a2dp_vendor_ldac(.cc/_encoder.cc)`、
  `a2dp_vendor_lhdcv2(.cc/_encoder.cc)`、`a2dp_vendor_lhdcv3(.cc/_encoder.cc)`、`a2dp_vendor_lhdcv5.cc`、
  **`a2dp_vendor_opus.cc`**、`a2dp_vendor_opus_decoder.cc`。
  函數字串包括 `A2DP_VendorSourceCodecIndex`、`A2DP_VendorInitCodecConfigLhdcV5`、`a2dp_vendor_opus_encoder_init`、`a2dp_vendor_opus_encoder_update` 等。
  這些字串多半來自日誌格式（`__func__`），**不代表有導出符號**；是否被 strip 還要用 `readelf` / `nm` 確認。
- Opus vendor codec 的程式碼在棧裡，但連線時的本機能力清單**沒有列出 Opus**，推測執行時被關閉。

### 小米 13 上的開關實測（已驗證）

- **Opus 槽位可以啟用**：`setprop persist.bluetooth.opus.enabled true` 並重開機後，本機能力清單出現
  `Opus`（codec type 6、優先度 1501、48 kHz / 16 bit / 立體聲），dumpsys 多了 `A2DP Opus State`。
  也就是說小米的分支保留了這個開關，而且 `is_opus_supported()` 的檢查會通過。Buds 不支援 Opus，所以不會被選中。
- **A2DP 硬體卸載被強制開啟**：`/vendor/etc/init/hw/init.qcom.rc:182` 有 `setprop persist.bluetooth.a2dp_offload.disabled false`，
  每次開機都把它設回 false（所以開發者選項的開關是灰色的）。開機前用 `setprop` 設 true 沒有用。
  好處是任何手動修改都會在重開機後自動恢復。
- 小米的卸載能力清單 `persist.bluetooth.a2dp_offload.cap = sbc-aac-aptx-aptxhd-ldac-lhdc` **不含 Opus**。
  選到 Opus 槽位時棧是否會自動改走軟體編碼，**尚未驗證**。

### AOSP 的 Opus vendor codec（來自 AOSP `packages/modules/Bluetooth` main 分支原始碼；小米的分支可能有改動）

- `a2dp_vendor_opus_constants.h`：`A2DP_OPUS_CODEC_LEN 9`、vendor `0x000000E0`、codec `0x0001`；
  設定位元組：聲道 mask `0x07`（MONO `0x01`、STEREO `0x02`、DUAL_MONO `0x04`）、幀長 mask `0x18`（10ms `0x08`、20ms `0x10`）、
  採樣率 mask `0x80`（48 kHz）。預設 48 kHz、每幀 960 樣本、2 聲道。
- **和 SSC 的形狀相同**：兩者的 Media Codec 長度都是 9（vendor ID 4 + codec ID 2 + **1 位元組**設定）。
  但**位元意義不同**：SSC 的 `0x3E` 沒有 Opus 要求的 `0x80`（48 kHz）位元，Opus 的解析邏輯會直接拒絕它，所以不能只改 vendor / codec ID。
- 編碼器直接呼叫 libopus（`opus_encoder_get_size`、`opus_encoder_init(48000, 2, OPUS_APPLICATION_AUDIO)`、`opus_encoder_ctl`、`opus_encode`），
  每次 960 樣本（20 ms），每個封包一幀，送幀節奏由計時器決定。SSC 是每幀 864 樣本，節奏不同。
- 碼率：`A2DP_VendorGetBitRateOpus` 回傳 256000（立體聲）。SSC 最高到 584k / 886k。
- libopus 的連結方式（靜態或動態）在 AOSP 的 `libbt-stack` 只看到 include 路徑，**正式版的連結方式未確認**。
- 執行時開關：`A2dpCodecs::init()` 讀 `persist.bluetooth.opus.enabled`（預設 false），且還要 `bluetooth::audio::a2dp::is_opus_supported()` 為真；
  編譯時受 `EXCLUDE_NONSTANDARD_CODECS` 控制。小米的分支是否保留這些條件，要實測。
- 音訊走 AIDL Bluetooth Audio HAL（`android.hardware.bluetooth.audio.IBluetoothAudioProviderFactory/default`，高通 `vendor.qti.hardware.bluetooth.audio-V1`）。
- **A2DP 硬體卸載已啟用**：`ro.bluetooth.a2dp_offload.supported=true`、`persist.bluetooth.a2dp_offload.disabled=false`，
  卸載能力 `sbc-aptx-aptxtws-aptxhd-aac-ldac-aptxadaptiver2`；與 Buds 連線時日誌顯示 `Stream sink offloaded`，
  也就是現在的 AAC 是 **DSP 編碼**。SSC 只能走軟體編碼，所以測試時必須先在開發者選項關閉「停用蓝牙 A2DP 硬體分流」那個選項（即關閉卸載）。
- 這個蓝牙棧**已經支援多個 vendor codec**：本機能力清單有 aptX-adaptive（type 7）、LHDCv5（19）、LHDC_V3（13）、LHDC_V2（12）、LDAC（4）、
  aptX-HD、aptX、AAC、SBC，以及小米自有的 **MIHC（type 20）**。codec type 已被擴充到 20，代表棧裡有現成的「新增 vendor codec」機制可參照。
- 軟體 LDAC 編碼器來自 VNDK APEX（`libldacBT_enc.so`）；LHDC 在 `/vendor/lib64/lib_bt_lhdc.so`；aptX 在 `/vendor/lib64/lib_bt_aptx.so`。

**與 Buds 3 Pro 的連線（Buds 連到小米 13）**
- 目前 codec：**AAC**，44.1 kHz / 16 bit / 立體聲，VBR。可選 codec 只有 AAC 與 SBC。
- 小米 13 上**沒有** `libScalable_Encoder.so`。

**btsnoop 中的 SSC 能力（關鍵發現）**
抓到的位元組（含前後文）：

```
11004300 320c 0100 0709 00ff 75000000 0301 3e 0800 ...
```

逐段解讀（依 AVDTP 結構，**是我手動解碼，建議再用 Wireshark 對照**）：

| 位元組 | 意義 |
|---|---|
| `11 00` `43 00` | L2CAP 長度 17、頻道 0x0043（AVDTP 信令） |
| `32 0c` | 交易 3、單一封包、**回應接受**、信號 `0x0C` = Get All Capabilities |
| `01 00` | 服務類別 Media Transport |
| `07 09` | 服務類別 **Media Codec**，長度 9 |
| `00 ff` | 媒體類型 audio、codec 類型 `0xFF`（vendor） |
| `75 00 00 00` | Vendor ID = `0x00000075`（Samsung） |
| `03 01` | Codec ID = `0x0103`（SSC） |
| **`3e`** | **vendor 專屬資訊，只有 1 位元組** |
| `08 00` | 服務類別 Delay Reporting |

結論：**Buds 3 Pro 對非三星的來源（小米 13）也回報了 SSC 端點**，能力位元組是 `0x3E`。
依上游解碼器 README 的模式旗標（第三方，未驗證）：`0x20/0x10/0x08` = SSC、`0x04` = HiFi、`0x02` = UHQ v2、`0x80` = 舊版 UHQ，
`0x3E` = 二進位 `0011 1110` = SSC + HiFi + UHQ v2 全部支援，與預期一致。
目前小米的棧不認識這個 codec，所以選擇時被略過，只剩 AAC 與 SBC。

**還需要確認**：Buds 對三星來源回報的位元組是否相同（需 S25+ 的 btsnoop）；`Set Configuration` 時三星手機實際送出的設定位元組
（尤其選 UHQ 96 kHz / 24 bit 時），這才是我們要複製的格式。

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
| ~~小米 13 的 HyperOS / Android 版本、蓝牙 APEX~~ | 已查：Android 17、`com.android.bt` 370399999 | 要對應 Android 17 的 AOSP 蓝牙原始碼；能否取得待確認 |
| `libbluetooth_qti.so` 在哪、裡面有沒有 `A2DP_Vendor*` / `a2dp_vendor_*` 框架 | `tools/probe-device.sh` v2（或手動 `find` + `grep`） | 決定走「改寫既有 vendor codec」還是「整個新增」 |
| 小米的棧是否 strip 了符號（能不能 hook / 對照） | 同上 | 決定方案 B 是否可行 |
| ~~Buds 3 Pro 的 SSC 能力位元組~~ | 已查：`0x3E`（見 1b） | — |
| Buds 對三星來源的能力位元組、以及三星手機送出的 `Set Configuration` 位元組 | S25+ 上的 btsnoop | 精確的設定格式（模式旗標、採樣率位元） |
| 關閉 A2DP 卸載後，Buds 是否能維持穩定的軟體編碼連線 | 小米 13 實測 | SSC 的必要前提 |
| APEX 唯讀的繞過方式在小米 13 + KernelSU 上哪個可行 | 實測 | 決定部署方式 |
