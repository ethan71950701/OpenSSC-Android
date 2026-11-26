# OpenSSC Probe 0.1（第一階段探測模組）

在小米 13 的 `com.android.bluetooth` 行程裡尋找設計文件列出的函數，寫成報告。
**不掛 hook、不改屬性、不改藍牙資料**，所以裝了耳機行為不會改變，也不會出現 SSC。

## 安裝（在電腦上、zip 所在的資料夾執行）

1. 手機接上電腦並開啟 USB 偵錯。在 KernelSU 的超級使用者清單允許「Shell」。
2. 傳送模組到手機：

```
adb push openssc-probe-0.1.zip /sdcard/Download/
```

3. 手機打開 KernelSU → 模組 → 安裝，選 `Download/openssc-probe-0.1.zip`。
4. 重開機，解鎖，開啟藍牙，等約 2 分鐘。
5. 取得報告：

```
adb shell "su -c 'cat /data/user_de/0/com.android.bluetooth/openssc_probe_report.txt'"
```

6. 報告不存在時，改看日誌：

```
adb logcat -d -s OpenSSCProbe
```

## 看結果

- `RESULT: PASS required=8/8`：第一版 hook 需要的函數都唯一找到。
- `RESULT: INCOMPLETE`：有函數找不到（MISSING）或有多個（AMBIGUOUS），把整份報告貼回來。
- 正常情況下約 60 秒後日誌會出現 `crash counter reset`。

## 停用

在 KernelSU 關掉模組後重開機；或用指令：

```
adb shell "su -c 'touch /data/adb/modules/openssc_probe/disable'"
```

```
adb reboot
```

開不了機時照 `docs/HANDOFF.md` 的救援步驟（安全模式或刷回不帶 ksu 的 `init_boot`）。

## 自動保護

- **藍牙閃退計數**：藍牙行程連續 3 次沒撐到穩定（約 60 秒），之後不再執行。
  短時間內手動重啟藍牙行程也會計入。重設指令：

```
adb shell "su -c 'rm /data/user_de/0/com.android.bluetooth/openssc_probe_state'"
```

- **開機計數**：連續 2 次開機沒完成，第 3 次自動停用。
- **緊急停用檔**：模組目錄有 `disable_probe` 就不執行。開機計數觸發後，要重新啟用需刪掉它：

```
adb shell "su -c 'rm /data/adb/modules/openssc_probe/disable_probe'"
```

## 建置與測試

```bash
ANDROID_NDK_HOME=/path/to/android-ndk-r28c ./build.sh
```

```bash
tests/run.sh
```

## 驗證狀態

- 已驗證（Linux）：用 NDK r28c 編譯成功；符號查找（含只存在 `.gnu_debugdata` 的函數）、損壞輸入、閃退計數的測試通過。
- **未驗證**：任何實機行為，包括 Zygisk 載入、SELinux 是否允許寫檔、開機腳本、小米符號的實際比對結果。
