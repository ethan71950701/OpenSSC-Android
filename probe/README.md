# OpenSSC Probe 0.2（第二階段：只記錄的 hook）

在小米 13 的 `com.android.bluetooth` 行程裡尋找設計文件列出的函數，寫成報告（和 0.1 一樣）。
**0.2 新增**：當 8 個必要函數都唯一找到時，用 ShadowHook（MIT，靜態連結，unique 模式）掛上「只記錄」的 hook。
每個 hook 都用原本的參數呼叫原函數、原樣回傳結果，只把看到的內容寫進日誌。**不改屬性、不改藍牙資料、不翻譯 codec、不換編碼器**，
所以耳機行為不會改變，也不會出現 SSC。

被掛的函數：`avdt_msg_prs_cfg`、`avdt_msg_bld_cfg`、`a2dp_vendor_opus_encoder_init / cleanup`、
`a2dp_vendor_opus_feeding_reset / feeding_flush`、`a2dp_vendor_opus_get_encoder_interval_ms`、`a2dp_vendor_opus_send_frames`，
加上選用的 `bta_av_co_audio_update_mtu`（找不到就略過）。日誌不記錄藍牙位址。

## 安裝（在電腦上、zip 所在的資料夾執行）

1. 手機接上電腦並開啟 USB 偵錯。在 KernelSU 的超級使用者清單允許「Shell」。
2. 傳送模組到手機：

```
adb push openssc-probe-0.2.zip /sdcard/Download/
```

3. 手機打開 KernelSU → 模組 → 安裝，選 `Download/openssc-probe-0.2.zip`。
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

報告（`openssc_probe_report.txt`）：
- `RESULT: PASS required=8/8`：第一版 hook 需要的函數都唯一找到。
- `RESULT: INCOMPLETE`：有函數找不到（MISSING）或有多個（AMBIGUOUS），這時不會掛任何 hook。

Hook 日誌（`openssc_probe_hooklog.txt`，每次藍牙行程啟動會重寫，最多約 800 行）：

```
adb shell "su -c 'cat /data/user_de/0/com.android.bluetooth/openssc_probe_hooklog.txt'"
```

- 開頭應有 9 行 `hooked ... at base+0x...`，最後是 `hooks: installed (log-only, original behaviour preserved)`。
- 失敗時會寫 `hooks: NOT installed ...` 或 `rolled back, nothing active`，並保持 0.1 的行為。
- 連上 Buds 時應出現 `avdt_msg_prs_cfg sig=0x0c ...`（Get All Capabilities 回應，含 Buds 的 SSC 能力）和
  `avdt_msg_bld_cfg wrote N bytes: ...`（棧實際送出的 Set Configuration 內容）。
- 播放時應出現 `opus_encoder_init`、`opus_send_frames #1/#2/#3`；目前 codec 是 AAC，
  所以**預期看不到** Opus 編碼器的行（Opus 槽位沒被選中）。要看到它們，需要先啟用 Opus 槽位並重開機（見 `docs/HANDOFF.md` 第 4 節）。
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

- **hook 專用開關**：模組目錄有 `disable_hooks` 檔案時只做探測、不掛 hook（0.1 行為）：

```
adb shell "su -c 'touch /data/adb/modules/openssc_probe/disable_hooks'"
```

- **串流閃退計數**：播放開始時記一次，連續播放約 30 秒或正常停止後歸零。連續 3 次在串流中閃退，之後不再掛 hook
  （狀態檔 `openssc_probe_stream_state`，和下面的閃退計數放在同一個資料夾）。

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

## 為什麼選 ShadowHook

- 文件原本就列它為候選；它是 MIT 授權、支援 arm64 inline hook（會重寫被覆蓋的指令），可以靜態連結進我們的 `.so`，
  不需要藍牙行程另外載入檔案（這是 0.1 交接文件列出的未驗證風險）。
- unique 模式：同一個函數只允許一個 hook，原函數指標可直接呼叫，沒有 shared 模式的堆疊記帳。藍牙行程裡沒有別的 hook 框架。
- 代價：`shadowhook_init` 會在行程內掛 linker 的 `call_constructors`，這是它支援「函式庫晚載入也能 hook」的做法；
  初始化失敗時整組 hook 不安裝（全有或全無）。

## 驗證狀態

- 已驗證（Linux）：`tests/run.sh` 通過，包含符號查找、損壞輸入、閃退計數，以及 hook 層的主機測試
  （用記錄器取代 ShadowHook：每個 hook 原樣轉送參數與回傳值、必要目標缺失或有多個時完全不安裝、
  中途失敗時全部回滾、串流閃退計數、日誌內容）。
- 已驗證：產物是 aarch64 共享庫，只匯出 `zygisk_module_entry`，所有未定義符號都在 bionic 的 `libc` / `libdl` / `liblog` 匯出表中。
- **這個 zip 不是用 NDK 建的**（雲端環境連不到 NDK 下載點）：用主機 clang 18 加 bionic、liblog 的標頭，
  libc / libm / libdl / liblog 用同名的空殼庫連結（只為了記錄 DT_NEEDED），`__clear_cache` 用 `src/clear_cache.c` 的實作。
  用 NDK 建置請照上面的指令；沒有 NDK 的環境見 `build.sh` 開頭。
- **未驗證**：任何實機行為，包括 ShadowHook 在小米 13 的 Android 17 藍牙行程中初始化與掛鉤、hook 後藍牙是否照常運作、
  Zygisk 之外的 SELinux 是否允許寫兩個新檔（`openssc_probe_hooklog.txt`、`openssc_probe_stream_state`）。
  `AvdtpSepConfig` 在小米版本的欄位配置未驗證，所以日誌只傾印原始位元組，不解讀欄位。
