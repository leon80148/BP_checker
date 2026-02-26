# BP_checker (ESP32 血壓機轉發器)

支援血壓機型號：目前預設 `OMRON-HBP9030`（可於網頁切換成 `CUSTOM` 解析模式）。

## 專案概述

`BP_checker` 是一個跑在 ESP32-S3 的血壓資料轉發器：
- 透過 TTL UART 接收血壓機資料
- 透過 WiFi 提供即時監控頁面
- 保存最近 20 筆歷史量測
- 提供原始資料檢視與基本異常標示

## 2026-02 UI/UX 改版重點

- 監控頁改為儀表板資訊層級（最新數值 -> 最近 5 筆 -> 原始資料收合）
- 統一 `/`、`/history`、`/config`、`/bp_model` 視覺語言
- 增加一致的頂部導覽與危險操作區塊（重置/清除）
- 保留每 3 秒整頁刷新（穩定優先）

詳細變更請看：[`docs/CHANGELOG.md`](docs/CHANGELOG.md)

## 硬體與連接

### 1. 血壓機資料線（TTL）

本專案程式使用：
- `RX: GPIO44`
- `TX: GPIO43`
- `9600 bps`

請注意必須是 **3.3V TTL**，不要直接接 RS232 電平。

### 2. 板子 USB 連接埠（OTG vs TTL）

- `OTG`：ESP32-S3 原生 USB。用於供電、燒錄、序列監看（本專案建議用這個 port 燒錄）
- `TTL`：UART/外設通訊用途。這個專案是拿來接血壓機資料，不是主要燒錄口

## 首次啟動

1. 上電後，若尚未配置 WiFi，ESP32 會開啟 AP：
   - SSID: `ESP32_BP_checker`
   - 密碼: `12345678`
2. 連上 AP 後打開 `http://192.168.4.1` 設定 WiFi
3. 連線成功後可從：
   - `http://bp_checker.local`
   - 或裝置取得的 IP

## 網頁功能

### `/` 監控儀表板

- 最新量測 KPI（收縮壓/舒張壓/脈搏）
- 最近 5 筆資料表
- 原始資料收合區塊
- 連線資訊與 WiFi 重置入口

### `/history` 歷史記錄

- 最近 20 筆歷史資料
- 每筆可查看原始資料（`/raw_data?id=<index>`）
- 可清除歷史記錄（含確認提示）

### `/config` WiFi 設定

- 掃描現場 WiFi 並選擇
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
arduino-cli compile -b esp32:esp32:esp32s3 /path/to/BP_checker
```

### 燒錄（OTG）

```bash
arduino-cli upload -b esp32:esp32:esp32s3 -p /dev/cu.usbmodemXXXX /path/to/BP_checker
```

## 常見問題

### 找不到序列埠

- 換可傳資料的 USB 線
- 優先使用板子的 OTG 口
- 直接插電腦，不經 Hub
- 需要時按 `BOOT + RST` 進下載模式

### 無法收到血壓機數據

- 確認血壓機接在 GPIO44/43（TTL）
- 確認共地與電平正確（3.3V TTL）
- 確認型號設定正確

### `bp_checker.local` 無法開啟

- 改用 IP 直接訪問
- 確認客戶端支援 mDNS
