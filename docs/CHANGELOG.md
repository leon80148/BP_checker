# Changelog

## 2026-03-14

### Documentation
- Defined the repository as an `ESP32-S3 + USB OTG Host + CDC` project.
- Added `docs/hardware.md` for the primary support matrix and hardware topology.
- Added `docs/fallback-uart.md` to isolate non-primary UART guidance.
- Added OTG host design and implementation planning documents under `docs/plans/`.

### Firmware
- Introduced a transport abstraction layer so parser/UI logic no longer depends directly on `Serial1`.
- Added explicit transport status reporting for the web UI and serial logs.
- Kept `UART fallback` as the default runtime mode while the OTG CDC host backend remains under migration.

## 2026-02-26

### UI/UX
- 監控頁改為儀表板式資訊層級：最新數值、最近 5 筆、原始資料收合。
- 統一 `/`、`/history`、`/config`、`/bp_model` 的視覺系統與導覽。
- 危險操作（重置 WiFi、清除記錄）改為明確的 danger 區塊。

### 開發維護
- 新增 `scripts/check_ui_markup.sh`，用於檢查核心 UI token/結構是否存在。
- 更新 `README.md`：補充 OTG/TTL 連接說明與開發燒錄流程。

## 2025-04-08

- 通訊接口由 USB 調整為 TTL 串口（RX: GPIO44, TX: GPIO43）。
- 優化原始數據顯示，修正中文顯示問題。
- 主頁新增最近 5 筆數據表格顯示。
- 優化頁面文案與顯示樣式。

## 2025-04-05

- 專案重組並更名為 `BP_checker`。
- 修正編譯錯誤與相容性問題。
- 歷史記錄容量提升至 20 筆。
- 新增台北時區（GMT+8）時間記錄。
