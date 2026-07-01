# Changelog

## 2026-07-02

### 測試基礎設施
- 新增 host-side 單元測試：`scripts/run_host_tests.sh` 以本機 clang++ 編譯
  header-only lib，`test/host/` 內含 Arduino String / Preferences shim 與
  fake MonitorTransport。覆蓋 BP_Parser、DataProcessor frame assembly、
  BPRecordManager 儲存層、WebSecurity，共 165 個檢查。

### 韌體
- DataProcessor 改為非阻塞 frame assembly：line-based 型號以換行為 frame
  邊界（CRLF 剝除），binary 型號以 30ms idle timeout flush；跨 loop 分段
  到達的 frame 不再被切成多筆垃圾記錄，也移除了阻塞 webserver 的等待迴圈。
- 持久化政策改為「只儲存解析成功的量測」：invalid frame 與超過 256 bytes
  的截斷 frame 只保留在 RAM 診斷區（減少 NVS 損耗、歷史不再出現 "—" 列）。
- `/config` 的 WiFi 掃描改為 async：頁面立即渲染上次結果，掃描於背景進行。

### 網頁安全
- `/reset`、`/configure`、`/set_bp_model`、`/clear_history` 加入 CSRF
  same-origin 檢查（Origin/Referer 與 Host 比對，跨源一律 403）。
- 儀表板不再顯示 AP 密碼。
- `/raw_data` 的 id 參數改嚴格解析，垃圾輸入回 400（原本會誤中 record 0）。
- 修正 history 頁「查看原始數據」對 invalid 記錄產生死連結的問題；
  `/api/history` 回應加入 `valid` 欄位。

## 2026-03-14

### Documentation
- Defined the repository as an `ESP32-S3 + USB OTG Host + CDC` project.
- Added `docs/hardware.md` for the primary support matrix and hardware topology.
- Added `docs/fallback-uart.md` to isolate non-primary UART guidance.
- Added OTG host design and implementation planning documents under `docs/plans/`.

### Firmware
- Introduced a transport abstraction layer so parser/UI logic no longer depends directly on `Serial1`.
- Added explicit transport status reporting for the web UI and serial logs.
- Added a compiled-in USB CDC OTG host backend under `src/`.
- Switched the default runtime mode to `USB OTG Host` on `ESP32-S3`.
- Kept `UART fallback` available through `lib/BPConfig.h`.

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
