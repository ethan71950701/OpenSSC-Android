# OpenSSC-Android

目標：讓 **Xiaomi 13（HyperOS，KernelSU root）** 透過 A2DP 以 **SSC（Samsung Scalable / Seamless Codec）** 連接 **Galaxy Buds 3 Pro**。
S25+ 只用來研究（取得協商資料、對照三星實作），不是目標裝置。

**新接手請從 [`docs/HANDOFF.md`](docs/HANDOFF.md) 開始讀。**

> 狀態：**設計已定，離線元件完成**。小米 13 的蓝牙棧已分析完（Opus 槽位可用、符號齊全，見 `docs/HOOK-DESIGN.md`），
> 轉換與組包的 `bridge/` 已有單元測試。SSC 設定位元組已由 S25+ 抓包確認（HiFi 48 kHz = `0x0C`、UHQ = `0x0A`）。注入蓝牙行程的模組尚未開始。

## 目前有什麼

| 項目 | 狀態 | 位置 |
|---|---|---|
| 開源 SSC 編碼器（第三方，BSD-3）的抓取、編譯、一致性驗證 | 已在 Linux 虛擬機驗證：137 次測試 0 失敗 | `scripts/` |
| 手機端編譯與自測（Termux） | 已在小米 13（ARM64）通過：`SELFTEST PASSED` | `scripts/build-tool.sh` |
| SSC↔Opus codec 資訊轉換與封包組裝（離線、可單元測試） | 已在 Linux 驗證：全部測試通過，含端對端編碼＋組包 | `bridge/`、`scripts/test-bridge.sh` |
| 手機蓝牙栈探測腳本（文字報告，不複製二進位檔） | v1 已在小米 13 跑過；v2 修正了目標庫並遮蔽 MAC | `tools/probe-device.sh` |
| btsnoop 的 SSC 協商與碼率分析（驗收用） | 已寫好，尚未用真實紀錄測試 | `tools/check-ssc-capture.sh` |
| 研究結論、已知事實與未知事項 | 見文件 | `docs/RESEARCH.md` |
| **接手指南：現況、剩下的工作、驗收標準、救援** | 入口文件 | `docs/HANDOFF.md` |
| 目前定案的整合設計與目標函數 | 設計 | `docs/HOOK-DESIGN.md` |
| 早期方案比較（歷史紀錄） | 歷史 | `docs/ARCHITECTURE.md` |

## 快速開始（PC 或 Termux）

```bash
scripts/build-tool.sh          # 抓取上游 → 編譯 ssc_enc_tool → 自測（需要 git、C 編譯器）
scripts/verify-encoder.sh      # 完整一致性測試（需要 cmake、python3）
scripts/test-bridge.sh         # bridge/ 的單元測試（需要 cmake）
sh tools/probe-device.sh       # 在手機的 Termux 裡執行，產生探測報告
```

## 重要原則

- **不要提交三星的二進位檔**（`libScalable_Encoder.so`、APEX、btsnoop 日誌等）。`.gitignore` 已擋掉常見副檔名。
- 優先使用**開源重寫的編碼器**，不要依賴三星的 `.so`。
- 所有結論都標示「已驗證 / 未驗證」；沒有實機驗證的，一律當成假設。

## 授權

本倉庫自己的程式碼待定（尚未選擇授權）。第三方編碼器為 BSD-3-Clause，由腳本在本機抓取，不隨倉庫散佈。
