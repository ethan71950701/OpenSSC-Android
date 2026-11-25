# OpenSSC-Android

目標：讓 **Xiaomi 13（HyperOS，KernelSU root）** 透過 A2DP 以 **SSC（Samsung Scalable / Seamless Codec）** 連接 **Galaxy Buds 3 Pro**。
S25+ 只用來研究（取得協商資料、對照三星實作），不是目標裝置。

> 狀態：**研究與工具階段**。蓝牙栈整合尚未開始，因為還缺小米 13 蓝牙栈的實測資料（見 `docs/HANDOFF.md`）。

## 目前有什麼

| 項目 | 狀態 | 位置 |
|---|---|---|
| 開源 SSC 編碼器（第三方，BSD-3）的抓取、編譯、一致性驗證 | 已在 Linux 虛擬機驗證：137 次測試 0 失敗 | `scripts/` |
| 手機端編譯與自測（Termux） | 腳本已寫，**尚未在手機上跑** | `scripts/build-tool.sh` |
| 手機蓝牙栈探測腳本（文字報告，不複製二進位檔） | 腳本已寫，**尚未在手機上跑** | `tools/probe-device.sh` |
| 研究結論、已知事實與未知事項 | 見文件 | `docs/RESEARCH.md` |
| 整合方案與需要做的事 | 設計稿 | `docs/ARCHITECTURE.md` |
| 換到本機電腦後怎麼接手 | 逐步清單 | `docs/HANDOFF.md` |

## 快速開始（PC 或 Termux）

```bash
scripts/build-tool.sh          # 抓取上游 → 編譯 ssc_enc_tool → 自測（需要 git、C 編譯器）
scripts/verify-encoder.sh      # 完整一致性測試（需要 cmake、python3）
sh tools/probe-device.sh       # 在手機的 Termux 裡執行，產生探測報告
```

## 重要原則

- **不要提交三星的二進位檔**（`libScalable_Encoder.so`、APEX、btsnoop 日誌等）。`.gitignore` 已擋掉常見副檔名。
- 優先使用**開源重寫的編碼器**，不要依賴三星的 `.so`。
- 所有結論都標示「已驗證 / 未驗證」；沒有實機驗證的，一律當成假設。

## 授權

本倉庫自己的程式碼待定（尚未選擇授權）。第三方編碼器為 BSD-3-Clause，由腳本在本機抓取，不隨倉庫散佈。
