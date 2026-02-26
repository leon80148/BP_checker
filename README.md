# BP_checker (ESP32-S3 血壓機 USB 轉 WiFi)

支援血壓機型號：預設 `OMRON-HBP9030`（可在網頁切換 `CUSTOM`）。

## 專案概述

`BP_checker` 是一個跑在 ESP32-S3 的血壓資料轉發器：
- 透過 **OTG USB Host (CDC)** 接收血壓機資料
- 透過 WiFi 提供即時監控頁面
- 保存最近 20 筆歷史量測
- 提供原始資料檢視與基本異常標示

## 2026-02 更新重點

- 血壓機資料通道改為 OTG USB Host（不再使用 TTL UART 收資料）
- AP/STA 流程強化：STA 失敗時自動切回純 AP
- AP 啟動時即開啟 HTTP 服務，避免等待 STA 期間網頁無回應
- UI 改版：`/`、`/history`、`/config`、`/bp_model` 統一視覺語言

詳細變更請看：[`docs/CHANGELOG.md`](docs/CHANGELOG.md)

## 硬體與接線（重要）

ESP32-S3 板上有兩個 USB-C：

- `TTL`：給電腦連線（供電 / 燒錄 / 序列監看）
- `OTG`：給血壓機 USB 線（資料接收）

建議接法：
1. `TTL` 接電腦（或可供電 USB）
2. `OTG` 接血壓機 USB

請勿把血壓機 USB 接到 `TTL` 口，`TTL` 不是 USB Host。

## WiFi 行為

- 沒有儲存 WiFi：進入純 AP 模式
- 有儲存 WiFi：使用 AP+STA（可同時保留 AP）
- STA 連線失敗：自動切回純 AP

AP 預設：
- SSID: `ESP32_BP_checker`
- 密碼: `12345678`
- 設定頁: `http://192.168.4.1`

## 首次啟動

1. 上電後連上 `ESP32_BP_checker`
2. 開啟 `http://192.168.4.1` 設定現場 WiFi
3. 之後可透過下列任一方式存取：
   - `http://bp_checker.local`
   - 裝置顯示的 LAN IP

## 網頁功能

### `/` 監控儀表板
- 最新量測 KPI（收縮壓/舒張壓/脈搏）
- 最近 5 筆資料
- 原始資料檢視區

### `/history` 歷史記錄
- 最近 20 筆歷史資料
- 每筆可查看原始資料（`/raw_data?id=<index>`）
- 支援清除歷史記錄

### `/config` WiFi 設定
- 掃描 WiFi 並選擇
- 支援手動輸入 SSID

### `/bp_model` 型號設定
- 可切換 `OMRON-HBP9030` / `CUSTOM`

## 開發與驗證

### UI 標記檢查
```bash
bash scripts/check_ui_markup.sh
```

### 編譯（ESP32-S3）
```bash
arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default /path/to/BP_checker
```
若出現 `missing BP_checker.ino`，請先把主檔更名為 `BP_checker.ino`，或在臨時資料夾複製後改名再編譯。

### 燒錄
```bash
arduino-cli compile --upload -b esp32:esp32:esp32s3 --board-options USBMode=default,UploadSpeed=115200 -p /dev/cu.usbserial-XXX /path/to/BP_checker
```

## 常見問題

### AP 連不上 / 設定頁打不開
- 先確認已連到 `ESP32_BP_checker`
- 直接開 `http://192.168.4.1`
- 若手機跳出「無網際網路」請選擇仍保持連線
- 看序列日誌是否有 `AP IP地址: 192.168.4.1`

### 無法收到血壓機數據
- 確認血壓機接在 `OTG` 口，不是 `TTL`
- 序列日誌應看到 `OTG已連接到CDC裝置`
- 目前資料格式依 `9600 8N1` CDC 設定讀取

### `bp_checker.local` 無法開啟
- 改用 LAN IP 直接訪問
- 確認客戶端網路支援 mDNS
