# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 專案性質

ESP32-S3 Arduino 韌體：透過 USB OTG Host 讀取血壓機資料，用 Wi-Fi 網頁介面提供即時查看與歷史記錄。單一 sketch (`bp_checker.ino`) + header-only 函式庫 (`lib/`)，沒有 `.cpp` 分檔，唯一例外是 `src/transports/UsbCdcTransport.cpp` 與其下 vendored 的 Espressif USB host CDC-ACM driver (`src/third_party/`)。

## 常用指令

```bash
# 編譯（OTG primary 模式）
arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default /path/to/BP_checker

# 編譯並上傳
arduino-cli compile --upload -b esp32:esp32:esp32s3 --board-options USBMode=default,UploadSpeed=115200 -p <PORT> /path/to/BP_checker

# UI 標記回歸檢查（改動 WebHandler.h 的 HTML/CSS/JS 後必跑）
bash scripts/check_ui_markup.sh

# Host-side 單元測試（改動 lib/ 下任何邏輯後必跑；本機 clang++，秒級完成）
bash scripts/run_host_tests.sh
```

**Sketch 檔名陷阱**：Arduino 要求主檔名 = 資料夾名。本資料夾為 `BP_checker`、主檔為 `bp_checker.ino`（大小寫不同）。macOS/Linux 編譯前可能要把主檔改名 `BP_checker.ino` 或資料夾改 `bp_checker`；Windows 不分大小寫可直接編譯。

`scripts/check_ui_markup.sh` grep 一組 CSS class、DOM id、API 路徑、JS 函式名是否仍存在於 `lib/WebHandler.h`。重構 UI 時若有意改名，必須同步更新該腳本的 token 清單。

`scripts/run_host_tests.sh` 編譯並執行 `test/host/test_*.cpp`（每檔一個獨立 binary）。`test/host/` 內含最小 shim：`Arduino.h`（String、fake millis/delay、Serial stub、可控 getLocalTime）、`Preferences.h`（in-memory fake NVS，`__reset()` 做測試隔離）。開發流程採 TDD：改 `lib/` 邏輯前先在對應 `test_*.cpp` 寫失敗測試。shim 只模擬被測程式實際用到的介面，勿為未用到的 API 擴充。

## 架構

**模組擁有權**：`bp_checker.ino` 的 `setup()` 持有所有全域狀態（`bp_model`、`lastData`、`transportName`、`transportStatus`、`server`、`preferences`）並 `new` 出各 manager，用指標傳入。Manager 之間不直接互相 new，都從 ino 注入依賴。

**資料流**（`loop()` 每輪）：
```
MonitorTransport.poll()/read()  →  DataProcessor（非阻塞累積進 256B frame buffer）
  →  frame 完成（line-based 型號遇 '\n'；binary 型號 30ms idle flush）
  →  BP_Parser.parse()（依型號選 parser，輸出 BPData）
  →  寫入全域 *lastData（HTML 包裝的 hex+ascii 診斷，永遠更新）
  →  只有 parse 成功（valid）才 BP_RecordManager.addRecord()
     （std::move 進 ring buffer + NVS；invalid/截斷 frame 不持久化）
WebHandler 各 route 讀全域 *lastData / recordManager 渲染 HTML/JSON
```

frame 邊界模式由 `BP_Parser::isLineDelimited()` 決定：binary 型號
（HBP1300/HEM7121/ES-P2020）的 payload 可能含 0x0A，不能用換行切。

**Transport 抽象**：`lib/transports/MonitorTransport.h` 是純虛介面（`begin/poll/available/read/name/state/detail`）。`kTransportMode`（`lib/BPConfig.h`）在 `setup()` 決定要 new 哪個實作：
- `TRANSPORT_MODE_OTG_PRIMARY`（預設）→ `UsbCdcTransport`（USB host，餵 vendored CDC-ACM driver）
- `TRANSPORT_MODE_UART_FALLBACK` → `UartTransport`（板子專屬 RX/TX，pin 也在 BPConfig.h）

新增血壓機型號：在 `BP_Parser` 加一個 `parseXxx()` 並在 `parse()` 的型號分派加 `else if`。`valid` 由 `parse()` 統一判定（SYS/DIA/PUL 皆 > 0）。`rawData` 欄位由 `DataProcessor` 填，不要在 parser 內重建。

**持久化**：`BP_RecordManager` 用 slot-based NVS 格式（`slot_<i>` + `count`/`index`/`schema=v2`），舊 `rec_*` 格式首次 load 會自動遷移。設計目的是把每筆 addRecord 的 NVS 寫入從 ~22 降到 4，減少 flash 損耗。`rawData`（原始 payload）**不持久化**：重啟後 history 該欄顯示 `—`，只有當次開機可見。

**Wi-Fi**：無 credential 進 AP 模式（`ESP32_BP_checker` / `192.168.4.1`）；有則連線並啟 mDNS (`bp_checker.local`)。`WiFiManager.tick()` 在 loop 偵測 STA 上線後才延遲啟動 mDNS。GPIO0 長按 3 秒清 ssid/password/admin_pin（實體救援路徑）、保留 bp_model。

**Web routes**（`WebHandler::setupRoutes`）：`/` = 即時監看頁、`/config` = Wi-Fi 設定、`/history`、`/bp_model`、`/raw_data`；API：`/api/latest`、`/api/history`。前端用 JS 每 3 秒 fetch `/api/latest` 就地更新 DOM，無 meta refresh。破壞性/設定變更 POST 走 `csrfBlocked()` + `pinBlocked()`（`lib/WebSecurity.h`：same-origin 檢查跨源 403；管理密碼 NVS `admin_pin`，未設定 = 放行）；`/set_pin` 設定/移除管理密碼；`/config` 的 WiFi 掃描是 async（頁面渲染上次結果，背景掃描）。

## 編碼慣例（重要）

近期 git 歷史幾乎全是 `perf:` commit，主題是**消滅 String 暫物件配置**。在此 codebase 寫程式時請延續：

- 渲染函式用 **append-into-`String& out`** 模式回傳 `void`，不要回傳 `String`。
- 回傳編譯期常數用 `const char*`，不要包成 `String`。
- 拼接用連續 `out += a; out += b;`，避免 `a + b + c` 產生中介暫物件。
- 大字串先 `reserve()`，size 對齊實際長度（見 commit 對 buildPageStart/sharedStyle 的調整）。
- hex 輸出用 lookup table，不要 `String(b, HEX)`。
- BPData 進 ring buffer 用 `std::move`，省 ~700B rawData copy。

所有使用者可控字串（SSID、型號名、BP 原始 payload）渲染進 HTML 前都要走 `htmlEscape()` / inline escape，已是既有做法，勿移除。

## 文件

- `docs/hardware.md` — 硬體與供電
- `docs/fallback-uart.md` — UART fallback 啟用方式
- `docs/CHANGELOG.md`、`docs/plans/` — OTG host 設計與實作計畫
- `docs/3d_print_case/` — 外殼 FreeCAD/3MF 檔
