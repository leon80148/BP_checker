# ESP32 血壓機轉發器使用手冊

## 一、產品概述

ESP32 血壓機轉發器是一款可以連接血壓機並通過WiFi網絡分享數據的設備。它可以讀取血壓機測量結果，並在網頁上實時顯示，同時保存歷史記錄以便追蹤健康趨勢。

## 二、基本安裝與設置

### 1. 硬件連接
- 將ESP32開發板連接到電源
- 使用RS232連接線連接ESP32和血壓機（RX連接Pin 16，TX連接Pin 17）

### 2. 首次啟動設置
- 首次開機時，設備會自動創建名為`ESP32_BP_checker`的WiFi熱點
- 使用手機或電腦連接此熱點，密碼為`12345678`
- 連接成功後，使用瀏覽器訪問`192.168.4.1`進入設置頁面

### 3. 配置WiFi連接
- 在設置頁面輸入您家中或辦公室的WiFi名稱和密碼
- 點擊「儲存並連接」按鈕
- 設備會重新啟動並嘗試連接到您設置的WiFi網絡

## 三、日常使用

### 1. 血壓數據監測
- 連接到與ESP32相同的WiFi網絡
- 在瀏覽器中輸入`http://bp_checker.local`或ESP32的IP地址
- 主頁面將顯示最新的血壓數據，包括收縮壓、舒張壓和脈搏

### 2. 進行血壓測量
- 確保ESP32已連接到血壓機且兩者均已開啟
- 按照血壓機的使用說明進行測量
- 測量完成後，數據會自動傳輸到ESP32並顯示在網頁上
- 系統會自動將數據保存到歷史記錄中

### 3. 查看歷史記錄
- 在主頁面點擊「查看歷史記錄」按鈕
- 系統會顯示最近測量的血壓數據列表（最多10筆）
- 超出正常範圍的數值會以紅色標記提醒注意

## 四、進階設置

### 1. 血壓機型號設置
- 點擊主頁面上的「型號設定」按鈕
- 從下拉菜單中選擇您的血壓機型號
- 支持的型號包括：OMRON HBP-9030、OMRON HBP-1300、OMRON HEM-7121、TERUMO ES-P2020
- 選擇完成後點擊「儲存設定」按鈕

### 2. 重置設備
- 如需重置WiFi設定，可以點擊頁面底部的「重置WiFi設定」
- 或長按ESP32上的重置按鈕（GPIO0）3秒以上
- 重置後設備會回到初始狀態，需要重新配置WiFi

### 3. 清除歷史記錄
- 在歷史記錄頁面點擊「清除記錄」按鈕
- 確認後將刪除所有保存的測量數據

## 五、故障排除

### 1. 無法連接WiFi
- 確認您輸入的WiFi名稱和密碼正確
- 確認WiFi信號強度足夠
- 如無法解決，可重置設備並重新配置

### 2. 無法讀取血壓數據
- 檢查ESP32與血壓機的連接線
- 確認血壓機型號設置正確
- 重啟血壓機和ESP32

### 3. 網頁無法訪問
- 確認您的設備與ESP32在同一WiFi網絡下
- 嘗試使用IP地址直接訪問
- 如使用`bp_checker.local`無法訪問，某些設備可能不支持mDNS服務

## 六、技術規格
- 處理器：ESP32-S3
- 通訊：WiFi 2.4GHz
- 與血壓機通訊：9600 bps (RX: Pin 16, TX: Pin 17)
- 數據存儲：最多保存10筆測量記錄
- 支持的血壓機型號：OMRON HBP-9030、OMRON HBP-1300、OMRON HEM-7121、TERUMO ES-P2020
