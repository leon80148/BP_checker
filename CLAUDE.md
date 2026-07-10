# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 專案性質

ESP32-S3 Arduino 韌體：透過 USB OTG Host 讀取血壓機資料，用 Wi-Fi 網頁介面提供即時查看與歷史記錄。單一 sketch (`BP_checker.ino`) + header-only 函式庫 (`lib/`)，唯一例外是 `src/transports/UsbCdcTransport.cpp` 與其下 vendored 的 Espressif USB host CDC-ACM driver (`src/third_party/`)。

## 常用指令

```bash
# 完整品質門（host、UI、warning-clean pinned firmware、SBOM）
bash scripts/run_quality_gate.sh

# 編譯並上傳
arduino-cli compile --upload -b esp32:esp32:esp32s3 --board-options USBMode=default,UploadSpeed=115200 -p <PORT> /path/to/BP_checker

# UI 標記回歸檢查（改動 WebHandler.h 的 HTML/CSS/JS 後必跑）
bash scripts/check_ui_markup.sh

# Host-side 單元測試（改動 lib/ 下任何邏輯後必跑；本機 clang++，秒級完成）
bash scripts/run_host_tests.sh
```

Arduino 主檔固定為 `BP_checker.ino`，必須與資料夾 `BP_checker` 大小寫完全一致。依賴版本與預設 build profile 由 `sketch.yaml` 鎖定。

`scripts/check_ui_markup.sh` grep 一組 CSS class、DOM id、API 路徑、JS 函式名是否仍存在於 `lib/WebHandler.h`。重構 UI 時若有意改名，必須同步更新該腳本的 token 清單。

`scripts/run_host_tests.sh` 編譯並執行 `test/host/test_*.cpp`（每檔一個獨立 binary）。`test/host/` 內含最小 shim：`Arduino.h`（String、fake millis/delay、Serial stub、可控 getLocalTime）、`Preferences.h`（in-memory fake NVS，`__reset()` 做測試隔離）。開發流程採 TDD：改 `lib/` 邏輯前先在對應 `test_*.cpp` 寫失敗測試。shim 只模擬被測程式實際用到的介面，勿為未用到的 API 擴充。

## 架構

**模組擁有權**：`bp_checker.ino` 的 `setup()` 持有所有全域狀態（`bp_model`、`lastData`、`transportName`、`transportStatus`、`server`、`preferences`）並 `new` 出各 manager，用指標傳入。Manager 之間不直接互相 new，都從 ino 注入依賴。

**資料流**（`loop()` 每輪）：
```
MonitorTransport.poll()/nextRxEvent() → DataProcessor / ProtocolFramer
  →  HBP-9030 exact CRLF frame（partial 永不以 idle time 完成）
  →  BP_Parser.parseResult()（format 5、設備時間、範圍與錯誤語意）
  →  寫入全域 *lastData（去識別化結構化診斷，不含 raw frame/subject ID）
  →  只有 parse 成功（valid）才 BP_RecordManager.addRecord()
     （std::move 進 ring buffer + NVS；invalid/截斷 frame 不持久化）
WebHandler 各 route 讀全域 *lastData / recordManager 渲染 HTML/JSON
```

frame 邊界由 `BP_Parser::framingContract()` 明確提供。Production 只支援 HBP-9030 format 5 `LINE_CRLF`；未驗證的 binary/CUSTOM 型號一律 fail closed。`ProtocolFramer` 的 synthetic fixed-frame 測試只驗證 header/checksum resync 狀態機，不代表支援其他血壓計。

**Transport 抽象**：`lib/transports/MonitorTransport.h` 以 ordered POD `MonitorRxEvent` 將 bytes/discontinuity 交給 main-loop owner，並保留 legacy byte adapter。`kTransportMode`（`lib/BPConfig.h`）在 `setup()` 決定實作：
- `TRANSPORT_MODE_OTG_PRIMARY`（預設）→ `UsbCdcTransport`（USB host，餵 vendored CDC-ACM driver）
- `TRANSPORT_MODE_UART_FALLBACK` → `UartTransport`（板子專屬 RX/TX，pin 也在 BPConfig.h）

新增血壓機型號前必須先有官方 wire grammar、可信 framing/resync contract、real-device corpus 與 HIL 驗收；不可加入 fallback parser。`BPData` 不得新增 subject ID 或 raw-frame 欄位。

**持久化**：`BP_RecordManager` 目前使用 slot-based NVS v2（`slot_<i>` + metadata），舊 `rec_*` 會遷移。任何版本都不得序列化 subject ID 或 raw frame；v3 crash-consistent slots 依 clinic-readiness plan 演進。

**Wi-Fi**：無 credential 進 AP 模式（`ESP32_BP_checker` / `192.168.4.1`）；有則連線並啟 mDNS (`bp_checker.local`)。`WiFiManager.tick()` 在 loop 偵測 STA 上線後才延遲啟動 mDNS。GPIO0 長按 3 秒清 ssid/password/admin_pin（實體救援路徑）、保留 bp_model。

**Web routes**（`WebHandler::setupRoutes`）：`/`、`/config`、`/history`、`/bp_model`、`/export.csv`，以及 `/api/latest`、`/api/history`。`/data` 只回傳去識別化診斷；不存在 raw-frame route。前端每 3 秒更新狀態，Wi-Fi 掃描採 async。

## 編碼慣例（重要）

近期 git 歷史幾乎全是 `perf:` commit，主題是**消滅 String 暫物件配置**。在此 codebase 寫程式時請延續：

- 渲染函式用 **append-into-`String& out`** 模式回傳 `void`，不要回傳 `String`。
- 回傳編譯期常數用 `const char*`，不要包成 `String`。
- 拼接用連續 `out += a; out += b;`，避免 `a + b + c` 產生中介暫物件。
- 大字串先 `reserve()`，size 對齊實際長度（見 commit 對 buildPageStart/sharedStyle 的調整）。
- production 不輸出 frame hex/ASCII；診斷只能使用固定 reason code 與去識別化欄位。
- BPData 進 ring buffer 使用 `std::move`，避免 timestamp String 複製。

所有使用者可控字串（例如 SSID、型號名）渲染進 HTML 前都要 escape。BP payload 不得直接渲染、記錄或匯出。

## 文件

- `docs/hardware.md` — 硬體與供電
- `docs/fallback-uart.md` — UART fallback 啟用方式
- `docs/CHANGELOG.md`、`docs/plans/` — OTG host 設計與實作計畫
- `docs/3d_print_case/` — 外殼 FreeCAD/3MF 檔
