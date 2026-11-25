# 交接指南：換到你本機電腦後怎麼接手

這個倉庫是在雲端虛擬機裡建立的，虛擬機**接不到你的手機**。以下步驟讓你在本機（有 adb 與手機）補上缺少的實測資料，並讓我之後直接在你的電腦上繼續。

## 0. 雲端已經完成的部分（不用重做）

- 開源 SSC 編碼器：編譯成功，上游 137 次一致性測試 0 失敗，我們的 18 項幀大小自測全過。
- S25+ 上的事實已整理在 `docs/RESEARCH.md`（含 `libScalable_Encoder.so` 的 SHA-256 與三星蓝牙棧的 SSC 字串）。
- 設計稿在 `docs/ARCHITECTURE.md`；**整合方案還沒定**，因為缺小米 13 的資料。

## 1. 取得倉庫

```bash
git clone https://github.com/ethan71950701/OpenSSC-Android
cd OpenSSC-Android
git checkout ccr-805a215d-bimx5e        # 目前所有內容都在這個分支
```

## 2. 在電腦上先確認工具鏈（5 分鐘）

需要：`git`、C 編譯器（gcc / clang）、`cmake`、`python3`、`adb`。

```bash
scripts/build-tool.sh        # 應該看到 "SELFTEST PASSED"
scripts/verify-encoder.sh    # 應該看到 "PASS"（會花幾分鐘）
adb devices                  # 兩支手機都要出現；手機上要開啟 USB 偵錯並授權
```

## 3. 在兩支手機上跑探測腳本（最重要）

腳本只輸出文字報告，**不複製任何二進位檔**。

1. 把腳本放到手機：
   ```bash
   adb push tools/probe-device.sh /sdcard/Download/
   ```
2. 在手機的 **Termux** 裡（先在 KernelSU 管理器授予 Termux root）：
   ```bash
   sh /sdcard/Download/probe-device.sh
   ```
3. 把報告拉回電腦，**放進 `research/private/`**（已被 git 忽略），先自己看一遍有沒有不想分享的內容：
   ```bash
   mkdir -p research/private
   adb pull /sdcard/Download/ssc-probe-<型號>-<時間>.txt research/private/
   ```
4. **S25+ 與小米 13 各跑一次**。

## 4. 在 S25+ 上抓 btsnoop（取得 SSC 協商位元組）

這一步是為了拿到 Buds 3 Pro 回報的 SSC 能力的**精確位元組**，是寫蓝牙棧程式碼的必要輸入。

1. 開發者選項 → 開啟「蓝牙 HCI 監控紀錄」。
2. 關閉再開啟藍牙（讓設定生效），把 Buds 3 Pro 斷線後**重新連線**，播放音樂 30 秒。
3. 若手機有顯示 SSC / UHQ 選項，分別以不同模式各連一次（普通 SSC、HiFi、UHQ），各抓一份。
4. 取出日誌（路徑依機型而異，先找）：
   ```bash
   adb shell su -c 'find /data/misc/bluetooth /data/log -iname "*snoop*"'
   adb shell su -c 'cp <找到的路徑> /sdcard/Download/btsnoop_ssc.log'
   adb pull /sdcard/Download/btsnoop_ssc.log research/private/
   ```
   探測腳本的 "HCI snoop log locations" 段也會列出可能的位置。
5. 用 Wireshark 開啟，看 **AVDTP** 封包：`Get Capabilities` 的回應與 `Set Configuration` 裡的 Media Codec，
   找 Vendor ID = `0x00000075`、Codec ID = `0x0103` 那一筆，**把完整的 codec info element 位元組抄下來**。
   （btsnoop 日誌檔本身不要提交；只把位元組貼進 `research/` 的筆記裡。）

## 5. 在小米 13 上驗證編碼器（可選，但建議）

在小米 13 的 Termux：

```bash
pkg install git clang
git clone https://github.com/ethan71950701/OpenSSC-Android && cd OpenSSC-Android
git checkout ccr-805a215d-bimx5e
scripts/build-tool.sh        # 看到 SELFTEST PASSED 代表 ARM64 上的編碼器行為正確
```

## 6. 把電腦接到這個對話，讓我直接動手

1. 在電腦安裝並開啟 **Claude 桌面應用**，進入 *設定 → Claude Code → Remote Control*，把這個倉庫資料夾加入。
2. 回到這個對話告訴我「電腦接好了」。我會呼叫 `list_computer_folders`，再請你批准 `request_computer_folder`；批准後我才能在你的電腦上執行指令。
3. 手機要用 USB 接在那台電腦上並通過 `adb devices`；我只能操作那台電腦上看得到的東西。
4. 批准後，我的指令與檔案工具會在你加入的那個資料夾所在的電腦上執行；請只加入這個倉庫資料夾，不要加入含有其他私人檔案的目錄。

## 7. 你把這些丟給我，我就能往下做

- [ ] 兩份探測報告（內容貼文字即可，不必傳檔）
- [ ] Buds 3 Pro 的 SSC codec info element 位元組（來自 btsnoop）
- [ ] 小米 13 的 HyperOS 版本、KernelSU 版本、是否解鎖 bootloader
- [ ] `build-tool.sh` 在小米 13 上的輸出（是否 `SELFTEST PASSED`）

## 8. 我拿到資料後會做什麼（依序）

1. 比對兩份報告，決定 `docs/ARCHITECTURE.md` 的方案 A / B / C。
2. 建立 vendor codec 的原型程式碼（參照 `a2dp_vendor_ldac.cc` 的結構）與 KernelSU 模組打包。
3. 在小米 13 上以 `adb logcat` 與 btsnoop 驗證協商與實際播放，逐步修正。
4. 更新 `docs/RESEARCH.md`，把「未驗證」改成「已驗證」或刪除。

## 注意事項

- 不要把 `libScalable_Encoder.so`、APEX、btsnoop 日誌提交進倉庫（`.gitignore` 已擋，但請再確認 `git status`）。
- 改動蓝牙棧有可能讓小米 13 的藍牙暫時失效；動手前先確認你知道怎麼停用 KernelSU 模組，並備份重要資料。
- 若 `su` 在 Termux 裡沒反應，先到 KernelSU 管理器確認 Termux 已被授權。
