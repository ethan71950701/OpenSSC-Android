# 接手指南

**目標**：讓 Xiaomi 13（HyperOS，KernelSU root）透過 A2DP 以 SSC（Samsung Scalable / Seamless Codec）連接 Galaxy Buds 3 Pro。

這份文件是給新接手的人的入口。先讀這份，需要細節時再看文末的「文件地圖」。

## 1. 現況速覽

| 部分 | 狀態 |
|---|---|
| 可行性研究 | ✅ 完成，所有關鍵未知都已用實機資料回答（見第 2 節） |
| SSC 編碼器 | ✅ 使用開源重寫版（BSD-3），已在 Linux 與小米 13（ARM64）上驗證 |
| SSC ↔ Opus 轉換與封包組裝（`bridge/`） | ✅ 完成並有單元測試，輸出與 S25+ 實際送出的位元組逐一相同 |
| 整合設計 | ✅ 定案，見 `docs/HOOK-DESIGN.md` |
| **在小米 13 蓝牙行程內的整合模組** | ❌ **尚未開始，這是剩下的工作** |
| 探測模組 0.1（只查找函數、寫報告，不掛 hook） | ⚠️ 已編譯並通過 Linux 測試，**尚未實機驗證**；見 `probe/HANDOFF-CLAUDE-CODE.md` |
| 驗收工具 | ✅ `tools/check-ssc-capture.sh`（分析 btsnoop；已在 S25+ 的真實紀錄上驗證） |

## 2. 已驗證的關鍵事實

1. **Buds 3 Pro 會對非三星手機回報 SSC**。小米 13 的 btsnoop 中，Buds 回報的 Media Codec 是 `07 09 00 ff 75 00 00 00 03 01 3e`
   （vendor `0x75`、codec `0x0103`、能力 `0x3E`），和對 S25+ 回報的完全一樣。
2. **三星送給 Buds 的設定位元組**（S25+ btsnoop，並以之後實際的幀碼率確認）：
   HiFi 48 kHz = **`0x0C`**（幀碼率代碼 `F3` = 192 kb/s）；UHQ 96 kHz = **`0x0A`**（`E3` = 584 kb/s）。
3. **小米 13 的蓝牙棧**是 `/apex/com.android.bt/lib64/libbluetooth_jni.so`（小米閉源分支 `MiuiBluetooth`）。
   只導出 2 個符號，但 `.gnu_debugdata`（MiniDebugInfo）裡有 24,817 個內部符號，Opus 槽位與 AVDTP 的函數都在裡面。
4. **Opus 槽位可以啟用**：`setprop persist.bluetooth.opus.enabled true` 後重開機，本機 codec 清單會出現 Opus（type 6）。
   它和 SSC 的 Media Codec 長度相同（9），是設計中借用的槽位。
5. **A2DP 硬體卸載**由 `/vendor/etc/init/hw/init.qcom.rc:182` 在每次開機強制開啟。執行中全域關閉卸載會讓棧切到軟體模式，**但沒有聲音**。
   設計因此不依賴全域關閉卸載（Opus 不在小米的卸載清單裡，預期會走軟體路線，**待驗證**）。
6. **開源編碼器**輸出與三星參考幀位元一致（上游作者的測試，我們重跑通過）；HiFi 192 kb/s 時每幀 484 位元組，幀頭碼率代碼 `F3`，與 S25+ 相同。

完整資料與來源：`docs/RESEARCH.md`。

## 3. 剩下的工作：整合模組

設計與目標函數在 `docs/HOOK-DESIGN.md`，以下是需求與驗收標準。

### 第一版範圍
- 只支援 **HiFi 48 kHz、192 kb/s**，送給 Buds 的設定位元組 **`0x0C`**。UHQ 留到之後。
- 借用小米蓝牙棧的 **Opus 槽位**：讓棧以為對方是 Opus，在 AVDTP 進出時做 SSC ↔ Opus 的轉換，並用開源 SSC 編碼器取代 Opus 編碼器。
- 轉換、組包與編碼都直接使用 `bridge/` 和上游編碼器，不要重寫。
- 以 KernelSU 模組（裝置上已有 Zygisk Next）的形式部署，只作用於 `com.android.bluetooth` 行程。

### 必要的安全要求
- **全有或全無**：任何一個必要的函數找不到或處理失敗，就完全不生效，讓藍牙照原本的方式運作。
- **依名稱在執行時解析**函數，不寫死位址（文件中的位址只對 OS4.0.31.0 這一版有效；部分符號名稱帶 `.llvm.*` 後綴）。
- **開機計數器**：連續幾次未完成開機就自動停用。**緊急停用檔**：放一個指定檔案即不生效。
- 不碰 zygote、system_server，不替換系統檔案或 APEX。

### 驗收標準（依序）
1. **協商**：在小米 13 上抓 btsnoop，`sh tools/check-ssc-capture.sh <紀錄>` 應看到 `ELEMENT 3e` 之後出現 `ELEMENT 0c`。
   參考：S25+ 的紀錄跑出來是 `ELEMENT 3e` → `ELEMENT 0c` → `ELEMENT 0a`（`rate e3` ×437）→ `ELEMENT 0c`（`rate f3` ×463）→ … → `ELEMENT 0c`（`rate f3` ×1979）。
2. **串流**：`ELEMENT 0c` 之後出現大量 `rate f3`（192 kb/s）。
3. **系統狀態**：`dumpsys bluetooth_manager` 顯示目前 codec 為 Opus（棧內部看到的名稱），且沒有新的 `Stream sink offloaded`。
4. **聽感**：Buds 有正常聲音，連續播放 5 分鐘以上不斷線、不爆音。
5. **回退**：停用模組並重開機後，回到 AAC，其他耳機不受影響。

## 4. 開發與測試環境

| 項目 | 值 |
|---|---|
| 目標手機 | Xiaomi 13（2211133C / fuxi），SM8550 |
| 系統 | 官改包，HyperOS `OS4.0.31.0.XEOCNXM`，Android 17（SDK 37），安全更新 2026-09-05 |
| 蓝牙 APEX | `com.android.bt` versionCode 370399999 |
| Root | KernelSU 32680；Zygisk Next 1.5.0（zygote64 / zygote32 皆已注入） |
| 研究用手機 | Galaxy S25+，One UI 8.5 / Android 16（臨時 root） |
| 耳機 | Galaxy Buds 3 Pro |

**在 PC 或 Termux 上重跑現有測試：**

```bash
scripts/build-tool.sh       # 抓取並編譯開源編碼器，跑 18 項幀大小自測
scripts/verify-encoder.sh   # 上游完整一致性測試（137 次）
scripts/test-bridge.sh      # bridge/ 單元測試與端對端測試
```

**手機上的常用操作：**

```bash
# 啟用 Opus 槽位（需重開機；恢復時設回 false）
su -c 'setprop persist.bluetooth.opus.enabled true'

# btsnoop：開發者選項開啟「藍牙 HCI 監控紀錄」後重開藍牙；小米的紀錄在
su -c 'find /data/misc/bluetooth -iname "*snoop*"'

# 目前 codec 與卸載狀態
su -c 'dumpsys bluetooth_manager' | grep -iE 'OffloadEnabled|Current Codec|sink offloaded'
```

- 經 `su` 執行 `logcat -d` 在這台手機上讀到 0 行；開發時請從電腦用 `adb logcat`。
- 探測腳本：`sh tools/probe-device.sh`，會產生文字報告並遮蔽 MAC。

## 5. 救援

出問題時依序嘗試：
1. **重開機**。卸載開關會被 `init.qcom.rc` 自動恢復。
2. **停用模組**：用 adb 在模組目錄放 `disable` 檔案後重開機，或使用 KernelSU 的安全模式（按法依版本而定，請先確認）。
3. **刷回原廠 `init_boot`**：使用官改包裡**不帶 ksu 的 `init_boot`**（`init_bootksu` 是打過 KernelSU 補丁的版本，刷了模組仍會載入）：
   ```bash
   fastboot flash init_boot init_boot.img
   ```
   這會暫時移除 KernelSU 與所有模組，不清除資料；之後再刷 `init_bootksu` 恢復 root。兩個檔案都必須和目前系統同一版本。

## 6. 資料衛生

- **不要提交**三星或小米的二進位檔（`.so`、APEX、解出的 `.gnu_debugdata`）或原始 btsnoop 紀錄；`.gitignore` 已擋常見副檔名，提交前仍請看 `git status`。
- btsnoop 與 `getprop` 輸出含藍牙位址；分享前遮蔽或只貼分析結果。原始報告放 `research/private/`（已忽略）。
- 抓包用完後，記得關閉兩支手機的「藍牙 HCI 監控紀錄」並刪除 `/sdcard/Download/` 裡的紀錄副本。

## 7. 已知陷阱

- **全域關閉 A2DP 卸載會沒有聲音**（見第 2 節第 5 點），不要用它當前提。
- **MTU**：AVDTP 媒體封包 = 12 位元組 RTP 標頭 + SSC 幀。HiFi 192 kb/s 一包 2 幀（980 位元組）；
  96 kHz 886 kb/s 一幀 1000 位元組，加標頭 1012，超過論壇提到的 1005。實際 MTU 要在連線時從棧取得，不能寫死。
- **內聯**：部分 vendor ID 比對被編進呼叫端，所以設計選擇在 AVDTP 邊界轉換，而不是只攔分派函數。
- **小米版本更新**後符號位址會變；只要函數名稱與簽名不變，依名稱解析的做法仍可用，但每次更新都要重新跑驗收。

## 8. 尚未解決的問題

| 問題 | 怎麼確認 |
|---|---|
| 卸載開啟時，選到 Opus 槽位的串流是否自動走軟體編碼 | 第一版模組實測（驗收標準 3） |
| AVDTP 媒體通道實際協商到的 MTU | 小米 btsnoop 中媒體通道的 L2CAP 設定 |
| UHQ 96 kHz 需要額外處理的採樣率與位元深度查詢 | 第一版穩定後再做 |
| 長時間串流的穩定性與耗電 | 實測 |

## 9. 文件地圖

| 文件 | 內容 |
|---|---|
| `docs/HANDOFF.md` | 本文件：現況、需求、驗收、救援 |
| `docs/HOOK-DESIGN.md` | **目前的整合設計**與目標函數表 |
| `docs/RESEARCH.md` | 所有實測資料與來源，標示已驗證 / 第三方 |
| `docs/ARCHITECTURE.md` | 早期的方案比較與決策過程（歷史紀錄） |
| `bridge/` | SSC ↔ Opus 轉換、封包組裝與測試 |
| `probe/` | 探測模組原始碼、測試與最新交接紀錄（`HANDOFF-CLAUDE-CODE.md`） |
| `scripts/` | 抓取 / 編譯 / 測試開源編碼器與 `bridge/` |
| `tools/probe-device.sh` | 手機蓝牙棧探測（文字報告） |
| `tools/check-ssc-capture.sh` | 從 btsnoop 列出 SSC 協商與碼率（驗收用）；S25+ 參考輸出見 `docs/RESEARCH.md` 1b |
| `research/` | 報告與筆記；`research/private/` 不進版控 |
