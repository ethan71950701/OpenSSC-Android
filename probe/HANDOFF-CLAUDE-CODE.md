# 交接：OpenSSC 探測模組（給電腦上的 Claude Code）

交接時間：2026-11-26（Asia/Taipei）

## 和使用者合作的方式
- 用繁體中文。技術操作每一步詳細說明。
- 要貼進終端機的指令，每條單獨放一個程式碼區塊。
- 重開機、刷機、改系統屬性、commit、push 之前先問使用者。

## 目標與背景
讓小米 13 透過 A2DP 以三星 SSC 連 Galaxy Buds 3 Pro。
倉庫：https://github.com/ethan71950701/OpenSSC-Android
交接時看到的最新 commit 是 `9fc1a1d`。
先讀倉庫的 `docs/HANDOFF.md`、`docs/HOOK-DESIGN.md`、`docs/RESEARCH.md`，再讀本檔和 `probe/README.md`。

目標機：小米 13（fuxi），HyperOS `OS4.0.31.0.XEOCNXM`，Android 17，KernelSU 32680，Zygisk Next 1.5.0。
S25+ 只做研究，不要把模組裝在上面。開發電腦是 Windows。

## 目前進度
- `probe/` 是新寫的探測模組 0.1，位於分支 `probe-module-0.1`。
- 它只在 `com.android.bluetooth` 行程查找函數並寫報告，不掛 hook、不改任何藍牙資料。
- 安裝包不進版控。雲端建好的 `openssc-probe-0.1.zip` 在使用者的對話下載裡（`openssc-handoff.zip` 的 `probe/dist/` 也有）；或自行建置。
- **尚未收到任何實機結果**。先問使用者是否已安裝。
- 使用者已確認：有同版本原廠 `init_boot`、電腦可用 `adb` / `fastboot`、系統沒更新過。

## 驗證狀態
- 已驗證（雲端 Linux，NDK r28c）：編譯成功；`tests/run.sh` 通過（符號查找、含只存在 `.gnu_debugdata` 的函數、
  ASan/UBSan 下的損壞輸入、閃退計數）；開機腳本只做過語法檢查。
- 已檢查：產物是 aarch64、16 KB 對齊、只匯出 `zygisk_module_entry`。
- 未驗證：Zygisk 載入（標頭是 API v5）、SELinux 是否允許寫狀態檔與報告、開機腳本、小米符號比對、Windows 上建置。

## 下一步
1. 安裝 0.1（步驟見 `probe/README.md`），重開機、開藍牙、等約 2 分鐘。
2. 取回報告：

```
adb shell "su -c 'cat /data/user_de/0/com.android.bluetooth/openssc_probe_report.txt'"
```

3. 判讀：
   - `RESULT: PASS required=8/8` → 做 0.2。
   - `INCOMPLETE` → 依 MISSING / AMBIGUOUS 修 `src/symres.cpp` 或 `src/module.cpp` 的目標清單。
   - 沒有報告 → 看 `adb logcat -d -s OpenSSCProbe`。完全沒有訊息就查 Zygisk 是否載入模組；
     `cannot access state file` 就查 SELinux 的 avc denied。
4. 0.2：加上只記錄、原樣呼叫原函數的 hook（AVDTP 解析 / 建構、Opus 編碼器、MTU）。函數簽名要先核對，不能猜。
5. 之後才做 SSC ↔ Opus 轉換與編碼器替換，驗收標準照倉庫 `docs/HANDOFF.md`。

## 0.2 以後要處理的風險（讀文件和程式碼得出，尚未實測）
- 硬體卸載開啟時，Opus 槽位是否走軟體編碼未知；全域關閉卸載的實驗是無聲的。這是最大風險。
- 轉換與編碼器替換要依連線裝置判斷是否為 SSC，否則會影響真正的 Opus 耳機。
- `avdt_msg_bld_cfg` 若也用於回覆手機自身能力，不能一律改成 SSC 設定值。
- 要實測小米的 codec 優先順序是否會選 Opus 槽位。
- Hook 庫尚未選定（設計提到 ShadowHook，未驗證）。藍牙行程能否從模組目錄載入額外 `.so` 也未驗證，優先考慮靜態連結。
- 現有的符號查找已能依名稱找內部函數（支援 `.llvm.*` 後綴），hook 時可直接用它給的位移。

## 建置
`build.sh` 目前寫死 `prebuilt/linux-x86_64`，在 Windows 上建議用 WSL 加 Linux 版 NDK r28c。
需要 `zip`；測試另需 clang、g++（含 ASan）、binutils、xz。以下在 `probe/` 資料夾執行：

```
ANDROID_NDK_HOME=/path/to/android-ndk-r28c ./build.sh
```

```
tests/run.sh
```

## 注意事項
- 不要提交三星或小米的二進位檔、原始 btsnoop。
- 保護機制：藍牙閃退計數（3 次）、開機計數、`disable_probe` 停用檔；加 hook 後仍要保留，並維持全有或全無。

## 緊急停用

```
adb shell "su -c 'touch /data/adb/modules/openssc_probe/disable'"
```

```
adb reboot
```

開不了機時照倉庫 `docs/HANDOFF.md` 第 5 節。
