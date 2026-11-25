# 整合方案：具體要做什麼

> 這是設計稿。凡標示「待確認」的地方，都要等 `tools/probe-device.sh` 在小米 13 上的報告和 S25+ 的 btsnoop 之後才能定案。

## 1. 要達成的行為

Android 的 A2DP 來源端流程是：

```
音訊 HAL / AudioFlinger ──PCM──▶ 蓝牙棧 (com.android.bt, libbluetooth.so)
                                   ├─ codec 選擇與協商 (AVDTP Get Capabilities / Set Configuration)
                                   ├─ 編碼器 (SBC / AAC / LDAC / ... )
                                   └─ 封包化 (RTP) ──▶ L2CAP ──▶ 耳機
```

要讓小米 13 以 SSC 傳給 Buds 3 Pro，蓝牙棧必須同時做到：

1. **宣告並選擇 SSC**：在 AVDTP 能力交換時，把 vendor `0x00000075` / codec `0x0103` 當作可用的來源 codec，並能解析 Buds 回報的 SSC 能力。
2. **用 SSC 編碼並封包**：每 864 個立體聲採樣編一幀，按 A2DP 的 RTP 格式送出。
3. **讓使用者能選到它**：開發者選項的「蓝牙音訊編解碼器」清單出現 SSC，並能設定採樣率 / 位元深度。

## 2. 元件

### A. 編碼器（已就緒）
開源 SSC 編碼器，C99，無靜態狀態，不是執行緒安全（每個實例各自獨立）。以靜態庫連入蓝牙棧即可。
- 輸入：864 個交錯立體聲採樣／幀；int16、24-bit（int32）或 float。
- 輸出：固定大小的幀（隨碼率與模式而定，見 `docs/RESEARCH.md`），幀內沒有長度欄位，接收端靠標頭推算。
- 驗證：`scripts/verify-encoder.sh`、`scripts/selftest.sh`。

### B. 蓝牙棧內的 SSC vendor codec（工作量最大，待確認）
需要新增（或改寫）等價於三星 `a2dp_vendor_ssc.cc` 的部分，參照 AOSP 的 `a2dp_vendor_ldac.cc` 結構：

| 要做的事 | 對應三星字串（S25+ 上看到的） |
|---|---|
| 組出 / 解析 codec info element（vendor 0x75、codec 0x0103、模式與採樣率位元） | `A2DP_BuildInfoSsc`、`A2DP_ParseInfoSsc` |
| 判斷來源 / 對端能力是否合法 | `A2DP_IsVendorSourceCodecValidSsc`、`A2DP_IsVendorPeerSinkCodecValidSsc` |
| 依對端能力調整設定 | `A2DP_VendorAdjustCodecSsc` |
| 提供編碼器介面（init / 送 PCM / 編碼 / 清理） | `A2DP_VendorGetEncoderInterfaceSsc`、`a2dp_ssc_encode_frames`、`a2dp_vendor_ssc_encoder_cleanup` |
| 每個封包塞幾幀 | `a2dp_ssc_get_num_frame` |
| 碼率、位元深度、採樣率查詢 | `A2DP_VendorGetBitRateSsc`、`A2DP_VendorGetTrackBitsPerSampleSsc` |
| 在 codec 清單與優先順序登記，Java 層的 codec 型別也要認得 | （AOSP 的 `a2dp_codec_config.cc`、`BluetoothCodecConfig`） |

**精確的 codec info element 位元組**（模式旗標、採樣率位元的排列）必須從 Buds 的真實回應取得，不能猜。見 `docs/HANDOFF.md` 的 btsnoop 步驟。

### C. 部署方式（待確認）
蓝牙模組是唯讀的 mainline APEX，有以下幾條路。要看小米 13 實際情況才能選：

| 方案 | 做法 | 優點 | 風險 |
|---|---|---|---|
| **A. 依來源重新編譯** | 取得和小米 13 蓝牙 APEX 相同版本的 AOSP `packages/modules/Bluetooth`，加入 SSC，編出 `libbluetooth.so`，用 KernelSU 模組覆蓋 | 程式碼清楚、好維護、可除錯 | 小米 / 高通可能在棧裡有私有修改，ABI 不一致就會崩潰；需要大型 AOSP 編譯環境 |
| **B. 二進位注入（hook）** | 以模組把共享庫注入蓝牙行程，hook 選碼與編碼流程 | 不需要整套 AOSP 環境 | 需要每個版本的偏移量；棧若被 strip 難度很高；非常脆弱 |
| **C. 改寫既有 vendor codec** | 若小米 13 的棧已有 LDAC / LHDC 等，把其中一個的識別碼與編碼器換成 SSC | 工作量可能最小 | 只有在該棧確實有可改寫的 codec 且符號可用時才成立；會犧牲被取代的 codec |

決策依據：探測報告裡 `libbluetooth_jni.so` 與 `libbluetooth_qti.so`（`ro.bluetooth.library_name` 指定的高通版）有沒有 `A2DP_Vendor*` 與 `a2dp_vendor_*` 字串、有沒有被 strip、蓝牙 APEX 的版本。

**小米 13 實測後的更新（見 `docs/RESEARCH.md` 1b）：**
- 這台是 Android 17，蓝牙 APEX 是 `com.android.bt` 370399999，A2DP 程式碼預期在 `libbluetooth_jni.so`（與 S25+ 同型態），**不是** `libbluetooth.so`。
- 小米的棧已有多個 vendor codec（LHDC V2/V3/V5、aptX-adaptive、LDAC）和自有的 MIHC（type 20），codec type 機制已被擴充過，
  所以「新增一個 vendor codec」在這個棧裡有現成的做法可以參照，方案 C（改寫既有的）不一定需要。
- Buds 對小米已經回報 SSC 端點（能力位元組 `0x3E`），所以**協商端不需要任何 Buds 端的繞過**；缺的只有小米的棧認得並使用它。
- 現在的 AAC 是 DSP 卸載編碼。SSC 只能軟體編碼，所以**必須關閉 A2DP 硬體卸載**，這是所有方案的前提，要先確認關閉後 Buds 仍能穩定連線。

**目前傾向：先評估 C（或在棧內新增 vendor codec），其次 A；B 只當最後手段。** 這只是傾向，不是結論，要等 `libbluetooth_qti.so` 的內容。

### D. KernelSU 模組（待確認）
最終要打包成一個 KernelSU 模組（`module.prop` + 覆蓋檔案 + 開機腳本）。目前**不建立空殼**，等 C 的方案定案再做，避免留下不能用的樣板。

## 3. 驗證方法（每一步都要有證據）

1. **編碼器正確性**（已完成，在主機上）：一致性測試 0 失敗。
2. **協商成功**：btsnoop 日誌中，小米 13 對 Buds 發出 `Set Configuration`，codec 為 vendor `0x75` / `0x0103`。
3. **系統認定**：`dumpsys bluetooth_manager` 與開發者選項都顯示 SSC。
4. **實際有聲音**：Buds 端實際播放，沒有爆音；放 5 分鐘以上確認不斷線。
5. **效能**：記錄 CPU 佔用、封包是否過大造成重傳（584k 與 886k 時一幀 > 1005 B，需要確認 L2CAP 切分行為）。
6. **回退**：模組停用 / 解除安裝後，系統回到原本的 codec，不影響其他耳機。

## 4. 已知風險

- **MTU 與大幀**：44.1 kHz 模式 584 kbps 一幀 1434 位元組、886 kbps 一幀 2172 位元組，都大於論壇提到的 1005 位元組軟體 MTU 上限。實際封包要怎麼切分，必須用 btsnoop 確認，不能從幀大小推論。
- **音效被繞過**：走軟體編碼時，高通 DSP 的音效處理不會套用（第三方說法，未驗證）。
- **系統更新**：OTA 更新蓝牙 APEX 後，覆蓋的檔案可能與新版不相容，要有一鍵停用的辦法。
- **崩潰風險**：蓝牙行程崩潰會導致整機蓝牙失效；開發過程要保留救援手段（KernelSU 模組可在安全模式或 adb 下停用）。
- **法律**：不散佈三星的二進位檔。使用開源重寫的編碼器；若引用三星的行為，只記錄觀察結果，不複製程式碼。
