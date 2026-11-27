# OpenSSC Probe 0.3（第三階段：只記錄的 hook ＋ 可選的靜音翻譯模式）

在小米 13 的 `com.android.bluetooth` 行程裡尋找設計文件列出的函數，寫成報告（和 0.1 一樣）。
**0.2 新增**：當 8 個必要函數都唯一找到時，用 ShadowHook（MIT，靜態連結，unique 模式）掛上「只記錄」的 hook。
每個 hook 都用原本的參數呼叫原函數、原樣回傳結果，只把看到的內容寫進日誌。**不改屬性、不改藍牙資料、不翻譯 codec、不換編碼器**，
所以耳機行為不會改變，也不會出現 SSC。

被掛的函數：`avdt_msg_prs_cfg`、`avdt_msg_bld_cfg`、`a2dp_vendor_opus_encoder_init / cleanup`、
`a2dp_vendor_opus_feeding_reset / feeding_flush`、`a2dp_vendor_opus_get_encoder_interval_ms`、`a2dp_vendor_opus_send_frames`，
加上選用的 `bta_av_co_audio_update_mtu`（找不到就略過）。日誌不記錄藍牙位址。

## 0.3 靜音翻譯模式（預設關閉）

模組目錄有 `enable_translate` 檔案時（重開機後生效）：
1. `avdt_msg_prs_cfg` 收到的能力清單裡，Buds 的 SSC 能力（vendor `0x75`、codec `0x0103`）在棧解析之前被改寫成 Opus 能力 `0x92`。
2. `avdt_msg_bld_cfg` 建好的 Set Configuration，如果是 48 kHz / 20 ms / 立體聲的 Opus 設定，在已寫好的位元組上改回 SSC `0x0C`。
   只在 Buds 能力被改寫後 30 分鐘內才改，且改的是送出的位元組，不碰棧自己的資料結構（所以不依賴 `AvdtpSepConfig` 欄位配置）。
3. 為了讓棧選到 Opus（它的優先度 1501 低於 AAC 的 2001，開發者選項選單也沒有 Opus），看過 Buds 的 SSC 能力之後 30 分鐘內，
   後續收到的能力清單裡的 AAC 會被標成無效 codec 類型（`TRANSLATE hid ... AAC`），棧就會忽略 AAC 而選 Opus。
   所以**第一次連線仍是 AAC，要把 Buds 斷線再連線一次才會走 Opus**。副作用：翻譯模式開著的 30 分鐘內，其他耳機也不會用 AAC（改用 SBC）。
4. 目前設定是 SSC 時，`a2dp_vendor_opus_send_frames` **不呼叫原函數**：沒有任何音訊封包送給 Buds（靜音）。
   之後只要有另一種 codec 的設定被建構（例如接其他耳機的 AAC）就解除靜音。

沒有 `enable_translate` 檔案時行為和 0.2.1 完全相同。要看到 Opus 槽位被選中，還需要 `persist.bluetooth.opus.enabled true` 並重開機。
開啟與回復步驟（在手機終端機執行）：

```
su -c 'setprop persist.bluetooth.opus.enabled true'
```

```
su -c 'touch /data/adb/modules/openssc_probe/enable_translate'
```

回復：

```
su -c 'rm /data/adb/modules/openssc_probe/enable_translate'
```

```
su -c 'setprop persist.bluetooth.opus.enabled false'
```

日誌關鍵字：`TRANSLATE caps`、`TRANSLATE config`、`MUTED`、`opus_encoder_init`。

## 0.3.4 診斷 hook（由 `diag_targets.txt` 決定，預設沒有）

模組目錄裡放一個 `diag_targets.txt`（一行一個函數名稱，`#` 開頭是註解，最多 10 個，格式 `函數` 或 `命名空間::函數`），
重啟藍牙後這些函數會被掛上「只記錄」的 hook，日誌出現 `DIAG 名稱(x0,...,x5) -> 回傳值`，每個函數最多記 40 次，指標只記值、不解讀。
**只適用於**最多 6 個整數／指標參數、沒有浮點數、沒有堆疊參數、不用隱藏指標回傳結構、不回傳浮點數的函數；
不符合的函數不要放（會破壞呼叫）。單一 diag hook 失敗只會被略過，不影響主要 hook。沒有這個檔案時完全不動作。

## 安裝（在電腦上、zip 所在的資料夾執行）

1. 手機接上電腦並開啟 USB 偵錯。在 KernelSU 的超級使用者清單允許「Shell」。
2. 傳送模組到手機：

```
adb push openssc-probe-0.3.4.zip /sdcard/Download/
```

3. 手機打開 KernelSU → 模組 → 安裝，選 `Download/openssc-probe-0.3.4.zip`。
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
- 正常情況下約 15 秒後（0.4.6 起；之前是 60 秒）日誌會出現 `crash counter reset`。

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

- **串流閃退計數**：串流開始（`encoder_init`）時記一次，行程撐過 60 秒、或正常停止後歸零（0.3.4 起；0.3.3 以前沒有實際播放就不會歸零，會累積到鎖死 hook）。被鎖住時 `openssc_probe_report.txt` 最後會有 `hooks: not installed (stream crash limit ...)`，刪掉 `openssc_probe_stream_state` 即可。連續 3 次在串流中閃退，之後不再掛 hook
  （狀態檔 `openssc_probe_stream_state`，和下面的閃退計數放在同一個資料夾）。

- **藍牙閃退計數**：藍牙行程連續 3 次沒撐到穩定（約 15 秒，0.4.6 起；之前 60 秒時快速關開藍牙會誤鎖），之後不再執行。
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

## 0.3.5 / 0.3.6

- 0.3.5：把「看過 SSC 耳機」記在 `openssc_probe_ssc_seen`（24 小時內有效）。藍牙程序重啟後第一次連線就隱藏 AAC，因為 Buds 的 AAC 能力比 SSC 先被讀到。
- 0.3.6：translate 模式下，擋下上層在 Opus 被選中約 9 秒後送來的「停用 Opus」請求（`A2dpCodecs::setCodecUserConfig`，type 6、priority -1），日誌記 `TRANSLATE blocked`。安裝時會保留舊版的 `enable_translate` 和 `diag_targets.txt`。
- 實測（全軟體路徑，見 `audiopolicy-module/README.md`）：Opus 槽維持住，串流開始，`opus_send_frames` 每 20 ms 一次（目前靜音）。下一步 0.4 在這裡換成真正的 SSC 編碼。

## 0.4.x：真正的 SSC 音訊

- 0.4.0：translate 模式下 SSC 設定生效時，`opus_send_frames` 不再靜音，改由 `ssc_sender` 讀取 48 kHz PCM，用開源編碼器（`third_party/samsung-ssc-encoder`，由 `scripts/fetch-upstream.sh` 下載）編成 192 kb/s 的 SSC 幀，照 Opus 編碼器的方式（`osi_malloc` 的 BT_HDR、時間戳放在 data[0..3]）交給協定堆疊；`A2DP_VendorBuildCodecHeader` 不加額外標頭。模組目錄放 `ssc_mute` 檔可恢復靜音。
- 0.4.1：每 100 個 tick 記一行音量軌跡（`SSC level`），用來區分音源斷音與傳輸斷音。
- 0.4.2：實驗用設定檔 `ssc_fpp`（每包幀數）和 `ssc_ts_step`（每幀 RTP 時間戳步進），只在藍牙程序啟動時讀取。
- 0.4.3 曾把預設改成每包 1 幀，但這是誤判：每包 1 幀時 SSC 串流約 1 秒就結束，系統退回 AAC，之後聽到的「正常」其實是 AAC。
- 0.4.4：預設改回每包 2 幀。每包 2 幀會規律斷音（約響 0.7 秒、停 1.5 秒），原因仍在查（待用 S25+ 的 btsnoop 比對）。
- 0.4.5：依 Buds 3 Pro 韌體的規則（參考 brunofgmag/SamsungScalableCodec-linux 的反組譯筆記：48 kHz 時每個 A2DP 封包只能有一個 SSC 幀），預設改回每包 1 幀，並把回報給堆疊的媒體 tick 從 20 ms 改成 18 ms（一幀 864 樣本），讓每個 tick 剛好送一包。0.4.3 每包 1 幀時串流約 1.3 秒就被暫停又重啟，推測是 20 ms tick 每次只取一包、佇列持續累積（未驗證）。設定檔 `ssc_interval_ms`（0 = 不改）。
- 0.5.0（實驗）：UHQ 模式。模組目錄放 `ssc_uhq` 檔時，設定位元組改送 `0x0A`（96 kHz），48 kHz PCM 經 2 倍半帶升頻（32 tap Blackman 視窗 sinc，20 kHz 以下誤差 < -63 dB）後以 96 kHz、584 kb/s 編碼（每幀 660 位元組、9 ms），每包 1 幀、媒體 tick 9 ms。音源仍是 48 kHz，所以 24 kHz 以上沒有內容；好處是較高的碼率。
- 0.5.1：translate 模式下，把協定堆疊裡 Opus 編碼槽的名稱改成 `SSC`（攔 `A2dpCodecConfig` 的基底建構子，只改 index 6 且原名為 "Opus" 的那個）。影響原生層回報的名稱（dumpsys 的原生區段、交給 Java 的 codec type 名稱）。Java 的 `BluetoothCodecConfig.getCodecName()` 和第三方 App 用 codec type 編號自己對照的名稱改不到，仍會顯示 Opus。
- 0.5.2：設定檔 `ssc_kbps` 指定碼率（kb/s）。HiFi 48 kHz 可用 88/96/128/192/229，UHQ（有 `ssc_uhq`）可用 152/291/584；其他值忽略。這些是 Buds 3 Pro 韌體認得的幀長度（參考 brunofgmag/SamsungScalableCodec-linux 的筆記）。
- 0.5.3：日誌行數上限 800 -> 4000（多次重連時 800 行會寫滿，看不到後面的音量軌跡），音量軌跡改成每 250 個 tick 記一行。

## 0.5.4

- The SSC display name no longer depends on start-up order. The constructor hook only sees configs built after it is installed; `A2dpCodecConfigOpusBase::setCodecConfig` and `setPeerCodecCapabilities` now also rename an already existing Opus config in place (codec index at +0x30, name string at +0x40).
- Note: SSC streaming needs "optional codecs" enabled in Developer options. With it disabled, the framework stops the stream about every 1.5 s.

## 0.5.5

- Diagnostics only. Diag lines for names containing `Pcm`, `setup_codec`, `UpdateAudioConfig` or `StartSession` also dump 48 bytes at the first two arguments (read through process_vm_readv, so a bad pointer cannot crash). Used to find the sample rate the stack hands the audio HAL, as groundwork for a real 96 kHz path.

## 0.5.6

- New `ssc_pcm_hz` file (48000 or 96000; 96000 needs `ssc_uhq`). The stack reported 44100 Hz for the A2DP PCM although the encoder assumed 48 kHz, and AudioFlinger's A2DP thread follows the reported rate ("dynamic rates"). The `getHalPcmConfiguration` hook now overwrites the sample rate with the file's value. With 96000 the sender takes the PCM as true 96 kHz (no upsampling).
- Delete the file and toggle Bluetooth to go back to the previous behaviour.

## 0.5.7

- Fix: the `ssc_pcm_hz` override was applied to every codec, so after a fallback to SBC the stream was fed 96 kHz data and played slowly. The override now applies only while the SSC configuration is active and the codec source rate is 48 kHz, and the sender uses true 96 kHz input only when the override really took effect.

### Status of `ssc_pcm_hz=96000` (experimental, off by default; with `ssc_pcm_bits=32` it is stable, see 0.5.8)

It reports 96 kHz to the audio HAL, so AudioFlinger delivers true 96 kHz PCM and the sender encodes it without upsampling. It worked in a first test, but later runs looped (about 0.9 s of playback, then AudioFlinger releases the A2DP patch and restarts), with every player. Without the file, UHQ 584 kb/s (Android resampling plus the built-in 2x upsampler) is stable and is the recommended setup (`ssc_uhq` only). Also keep "optional codecs" enabled in Developer options; toggling it off and on is the known fix when the stream starts looping.

## 0.5.8

- Experimental `ssc_pcm_bits` file (value 32, only together with `ssc_pcm_hz=96000`): also asks the audio HAL for 32-bit PCM, as the Samsung stack does (864 samples x 2 channels x 4 bytes = 6912 bytes per 9 ms frame). The sender reads int32 and encodes from floats. Off by default.
