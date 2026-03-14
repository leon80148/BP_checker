# BP_checker（ESP32-S3 USB OTG Host 血壓機橋接器）

支援對象：具備可用 `USB OTG/Host` 路徑的 `ESP32-S3` 開發板。

## 這是什麼

`BP_checker` 會在 `ESP32-S3` 上執行，透過 `USB OTG Host` 讀取血壓機資料，並用 Wi-Fi 網頁介面提供即時查看與歷史記錄。

這個專案目前採用 `OTG-first` 設計：

- 預設資料通道是 `USB OTG Host`
- 血壓機需能枚舉成相容的 `USB CDC` 類序列裝置
- `UART fallback` 仍保留，可在 `lib/BPConfig.h` 切換
- 目前使用中的 transport 會顯示在 serial log 與 Web UI

如果你的板子不支援 OTG Host，或你的血壓機只能走直接串列接線，請改看：

- [`docs/fallback-uart.md`](docs/fallback-uart.md)

## 主要支援範圍

只有在下列條件都成立時，才算是本專案的主要支援路徑：

- MCU 為 `ESP32-S3`
- 板子真的有可用的 `USB OTG/Host` 連接方式
- OTG 端供電足夠，或你使用的是已驗證的外接供電拓樸
- 血壓機會枚舉成相容的 `USB CDC` 裝置

不符合這些條件的板子或接法，不屬於主要支援範圍。

更多硬體說明請看：

- [`docs/hardware.md`](docs/hardware.md)

## 快速開始

### 接線方式

主要接法如下：

1. 將板子的 **燒錄/供電 USB** 接到電腦或穩定電源。
2. 將血壓機的 USB 線接到板子的 **OTG/Host USB**。

> ✅ **最佳做法**：主路徑不需要額外接 `GPIO RX/TX` 資料線。

### 第一次開機

1. 幫板子上電。
2. 用手機或電腦連上 Wi-Fi：`ESP32_BP_checker`。
3. 打開 `http://192.168.4.1`。
4. 如果需要，先設定要連接的現場 Wi-Fi。

成功判定：

- 你能打開首頁
- 你能看到裝置頁面與 transport 狀態

設定完成後，可改用以下任一方式存取：

- `http://bp_checker.local`
- serial log 顯示的區網 IP

## 常見任務

### 查看最新量測資料

打開首頁 `/`，你會看到：

- 最新一筆 `SYS / DIA / PUL`
- 最近量測資料
- 原始資料區塊

### 查看歷史記錄

打開 `/history`，你會看到：

- 最近 20 筆記錄
- 原始 payload 明細
- 清除歷史記錄操作

### 設定 Wi-Fi

打開 `/config`，你可以：

- 掃描附近 Wi-Fi
- 選擇現有 SSID
- 手動輸入 SSID

### 切換血壓機型號

打開 `/bp_model`，目前提供：

- `OMRON-HBP9030`
- `CUSTOM`

## 開發

### 檢查 UI 標記

```bash
bash scripts/check_ui_markup.sh
```

### 編譯

```bash
arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default /path/to/BP_checker
```

如果 Arduino sketch 資料夾名稱和 sketch 主檔名稱不一致，請先把主檔改成 `BP_checker.ino`，並放在 `BP_checker` 目錄下再編譯。

### 上傳

```bash
arduino-cli compile --upload -b esp32:esp32:esp32s3 --board-options USBMode=default,UploadSpeed=115200 -p <PORT> /path/to/BP_checker
```

## 卡住了怎麼辦

### 網頁打得開，但沒有血壓機資料

- 確認板子是有實際 OTG Host 能力的 `ESP32-S3`
- 確認血壓機接的是 **OTG/Host** 口，不是只有燒錄用途的 USB 口
- 確認血壓機會枚舉成相容的 `USB CDC` 裝置
- 查看 serial log 是否出現 attach、unsupported-device、disconnect 或 no-data 狀態
- 如果 firmware 顯示 `UART fallback`，代表你現在應該改走板子專屬的串列接線，而不是 OTG

> 🧯 **卡住了？** 先確認 OTG 端是否真的有供電，再確認血壓機 USB 介面是否為標準 CDC。這兩點是最常見根因。

### `bp_checker.local` 打不開

- 直接改用區網 IP 存取
- 確認你的網路環境支援 mDNS

### 我的板子不支援 OTG Host

改走 fallback 路徑：

- [`docs/fallback-uart.md`](docs/fallback-uart.md)

## 3D 列印外殼檔案

開發版外殼檔案放在 `docs/3d_print_case/`：

- `docs/3d_print_case/2.FCStd`：FreeCAD 原始模型
- `docs/3d_print_case/bp_checker_case.3mf`：3MF 列印檔
