# Hook 設計：讓小米 13 的 Opus 槽位說 SSC

> 依據：小米 13（HyperOS OS4.0.31.0、`com.android.bt` 370399999）的 `libbluetooth_jni.so` 的 MiniDebugInfo 符號表（24,817 個符號）。
> 位址只對這個版本有效；實作時一律**在執行時依名稱解析**，不寫死位址。

## 1. 已確認的事實

- `libbluetooth_jni.so` 只導出 2 個動態符號，但 `.gnu_debugdata` 裡有完整的內部函數名稱與位址。
- **Opus 槽位的函數全部有符號**（見第 4 節），而且槽位可以用 `persist.bluetooth.opus.enabled=true` 打開（已驗證）。
- **libopus 是靜態連結**在 `libbluetooth_jni.so` 裡（`opus_encode` 等都在這個庫內），所以不能用替換 `libopus.so` 的方式，必須 hook。
- vendor 分派函數有符號：`A2DP_VendorSourceCodecIndex`、`A2DP_VendorCodecTypeEquals`、`A2DP_VendorGetEncoderInterface`、
  `A2DP_VendorCodecInfoString`、`A2DP_VendorBuildCodecHeader`、`A2DP_VendorUsesRtpHeader`、`A2DP_VendorCodecGetCodecId`、`A2DP_VendorInitCodecConfig`。
- **有些函數被內聯了，沒有獨立符號**：例如 `A2DP_IsVendorPeerSinkCodecValid`、`A2DP_VendorCodecGetVendorId`。
  這代表「比對 vendor ID」的邏輯可能被編進呼叫端，只 hook 分派函數可能攔不全。

## 2. 設計：在 AVDTP 邊界翻譯，讓棧以為對方是 Opus

因為有內聯，最穩的做法是**不讓棧看到 SSC**：

```
Buds  ──(SSC 能力: 75 00 00 00 / 03 01 / 3E)──▶  [翻譯層] ──(Opus 能力: E0 00 00 00 / 01 00 / 92)──▶  小米蓝牙棧
Buds  ◀─(SSC 設定: 75 00 00 00 / 03 01 / ??)──  [翻譯層] ◀─(Opus 設定)──────────────────────────────  小米蓝牙棧
                                                                                    │
                                            Opus 編碼器 ──被替換成──▶ 開源 SSC 編碼器（864 樣本／幀）
```

1. **能力翻譯（收）**：Buds 回報的 SSC 端點，在棧解析前改寫成合法的 Opus 能力（48 kHz｜20 ms｜立體聲 = `0x80|0x10|0x02 = 0x92`）。
   棧內部所有內聯的比對看到的都是 Opus 的常數，自然會選這個槽位。
2. **設定翻譯（送）**：棧送出的 Opus `Set Configuration` 在出去前改回 SSC 格式。
   設定位元組已從 S25+ 的 btsnoop 取得：HiFi 48 kHz = `0x0C`、UHQ 96 kHz = `0x0A`。
3. **編碼器替換**：hook Opus 編碼器的 init / send_frames / feeding / cleanup，
   在 init 時保存棧傳入的 PCM 讀取回呼與封包送出回呼，之後由我們自己讀 PCM、用 SSC 編碼、組 RTP 封包送出。
   幀長 864 樣本（48 kHz 為 18 ms），節奏由我們控制，不沿用 Opus 的 20 ms。
4. **第一版做 HiFi 48 kHz、192 kb/s**，送給 Buds 的設定位元組是 **`0x0C`**（與 S25+ 實際送出的完全相同，已由抓包確認）。
   棧對 Opus 槽位只認 48 kHz / 16 bit，所以音訊 HAL 會送 16 bit PCM；編碼前乘以 256 當作 24 bit 輸入（和三星 HiFi 路徑相同）。
   每幀 484 位元組，MTU ≥ 980 時一包 2 幀。`bridge/` 的端對端測試就是照這個設定跑的，幀頭碼率代碼為 `F3`，與 S25+ 相同。
   UHQ（96 kHz / 24 bit）要另外 hook 採樣率與位元深度的查詢，放到之後。

翻譯層的掛載點（符號已確認）：
- **`avdt_msg_prs_cfg(AvdtpSepConfig*, uint8_t* p, uint16_t len, uint8_t* p_elem, uint8_t sig_id)`**（`0xa38614`）：
  解析收到的能力與設定（Get Capabilities 回應、Set Configuration 命令都經過這裡）。在它解析完之後把 SSC 元素改寫成 Opus。
- **`avdt_msg_bld_cfg(uint8_t**, AvdtpSepConfig*)`**（`0xa37e1c`）：建構要送出的設定（Set Configuration / Reconfigure）。在它建構之前把 Opus 元素改回 SSC。
- 備用的上層掛載點：`bta_av_co_audio_getconfig`（`0x676678`）、`bta_av_co_audio_setconfig`（`0x678ad8`）、`bta_av_co_save_codec`（`0x67a1d4`）。

轉換邏輯本身已寫成與平台無關的 `bridge/`（`sscb_peer_caps_to_opus`、`sscb_opus_config_to_ssc`），在 PC 上有單元測試。

## 3. 注入方式

- 只注入 **`com.android.bluetooth` 行程**（不碰 zygote、system_server）。
- 需要 Zygisk（KernelSU 上用 Zygisk Next、ReZygisk 或 NeoZygisk 之類的實作）；或 LSPosed/Vector 以 Java 端載入我們的原生庫。
- Inline hook 庫候選：ShadowHook（MIT）。我記得它底下的 xDL 能從 `.gnu_debugdata` 查非導出符號，並支援函式庫載入後才 hook，**需要實測確認**。
- 安全機制（見 `docs/ARCHITECTURE.md`）：開機計數器、緊急停用檔、所有 hook 安裝失敗時整個模組不生效（全有或全無，不留半套 hook）。

## 4. 目標函數（此版本的位址，僅供參考）

**Opus codec 資訊與設定**

| 函數 | 位址 |
|---|---|
| `A2DP_ParseInfoOpus(tA2DP_OPUS_CIE*, const uint8_t*, bool)` | `0xa035f0` |
| `A2DP_BuildInfoOpus(uint8_t, const tA2DP_OPUS_CIE*, uint8_t*)`（`.llvm.*` 後綴） | `0xa0502c` |
| `A2DP_IsCodecValidOpus` | `0xa03560` |
| `A2DP_IsVendorSinkCodecSupportedOpus` | `0xa03940` |
| `A2DP_VendorCodecTypeEqualsOpus` / `A2DP_VendorCodecEqualsOpus` | `0xa03b60` / `0xa03c70` |
| `A2DP_VendorGetBitRateOpus` | `0xa03dac` |
| `A2DP_VendorGetFrameSizeOpus` | `0xa04034` |
| `A2DP_VendorGetTrackSampleRateOpus` / `ChannelCount` / `BitsPerSample` | `0xa041cc` / `0xa03ef0` / `0xa042bc` |
| `A2DP_VendorCodecInfoStringOpus` | `0xa045f4` |
| `A2dpCodecConfigOpusBase::setCodecConfig` | `0xa05350` |
| `A2dpCodecConfigOpusBase::setPeerCodecCapabilities` | `0xa0593c` |
| `A2dpCodecConfigOpusSource::A2dpCodecConfigOpusSource(priority)` | `0xa05274` |

**Opus 編碼器**

| 函數 | 位址 |
|---|---|
| `a2dp_vendor_opus_encoder_init(peer_params, config, read_cb, enqueue_cb)` | `0xa062a8` |
| `a2dp_vendor_opus_encoder_cleanup` | `0xa06234` |
| `a2dp_vendor_opus_feeding_reset` / `feeding_flush` | `0xa068ac` / `0xa06934` |
| `a2dp_vendor_opus_get_encoder_interval_ms` | `0xa06910` |
| `a2dp_vendor_opus_get_effective_frame_size` | `0xa06d8c` |
| `a2dp_vendor_opus_send_frames(uint64_t timestamp_us)` | `0xa06944` |
| `a2dp_opus_encode_frames(uint8_t nb_frame)` | `0xa069e0` |
| `a2dp_vendor_opus_set_transmit_queue_length` | `0xa06d7c` |

**vendor 分派**

| 函數 | 位址 |
|---|---|
| `A2DP_VendorSourceCodecIndex` | `0x9e8f74` |
| `A2DP_VendorCodecTypeEquals` | `0x9e88c8` |
| `A2DP_VendorGetEncoderInterface` | `0x9e8d40` |
| `A2DP_VendorBuildCodecHeader` | `0x9e8b9c` |
| `A2DP_VendorUsesRtpHeader` | `0x9e8884` |
| `A2DP_VendorCodecGetCodecId` | `0x8db3b4` |
| `A2DP_SourceCodecIndex` | `0x8df6d0` |

## 5. 待確認

| 項目 | 怎麼查 |
|---|---|
| ~~AVDTP 收發與 `bta_av_co` 的符號~~ | 已確認：`avdt_msg_prs_cfg` / `avdt_msg_bld_cfg` |
| ~~三星送給 Buds 的 SSC 設定位元組~~ | 已確認：HiFi 48 kHz = `0x0C`、UHQ 96 kHz = `0x0A` |
| 選到 Opus 槽位時是否自動走軟體編碼（卸載仍開啟時） | 第一個原型實測 |
| ~~裝置上可用的 Zygisk 實作~~ | 已確認：Zygisk Next 1.5.0（KernelSU 32680），zygote64 / zygote32 皆已注入 |
| ShadowHook 能否解析 `.gnu_debugdata` 符號 | 原型實測 |

## 6. 封包與 MTU

- AVDTP 在每個媒體封包前加 12 位元組的 RTP 標頭；依上游說明（第三方），SSC 封包是「RTP + 1～2 幀」，沒有額外的 payload 標頭。
- **論壇說的「MTU 1012 > 1005」可以由此推算出來**：96 kHz UHQ 886 kb/s 一幀 1000 位元組，加上 12 位元組 RTP 標頭正好是 1012，超過 1005。
  這和「SSC 封包沒有額外標頭」的說法吻合（間接佐證，非直接驗證）。所以 96 kHz 在 1005 的 MTU 下最高只能用 584 kb/s（660 + 12）。
- 48 kHz 模式 192 kb/s 一幀 484 位元組，一個封包可放 2 幀（980 ≤ 1005）。`bridge/` 的 `sscb_packer` 依 MTU 計算每包幀數，並以每幀 864 推進 RTP 時間戳。
- 實際的 AVDTP 媒體 MTU 要在連線時從棧取得（`bta_av_co_audio_update_mtu`），不能寫死 1005。
