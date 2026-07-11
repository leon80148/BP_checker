# 診所部署與操作

## 發布前提

本裝置是診所內部、不保存病患身分的量測閘道，不是診斷系統。正式部署只接受
OMRON HBP-9030 後方 USB2 Type-B、功能項目 32 設為格式 5。參考硬體、供電限制
與實機檢查表見 [`hardware.md`](hardware.md)。沒有當次 device-in-loop 與 soak
證據時，不得把該映像標示為已完成診所驗證。

部署網路必須是診所管理的 WPA2 以上隔離網段；目前 HTTP Basic authentication
不加密同網段流量。若網路會承載病患識別資料，請停止部署並先提供 HTTPS 或受信任
TLS gateway。

## 唯一秘密啟用

每台裝置第一次開機會由 CSPRNG 分別產生 AP、一次性啟用碼、`admin` 與 `staff`
秘密，不使用 MAC、chip ID 或共用預設密碼。

1. 在裝置旁以實體 serial console 讀取一次性的
   `commissioning_ap_password` 與 `commissioning_bootstrap_token`。
2. 連上 `ESP32_BP_checker`，開啟 `http://192.168.4.1/claim`。
3. 輸入一次性啟用碼；將產生的 admin、staff 與復原 AP 秘密立即存入診所核准的
   密碼管理器。不要拍照、貼標籤或傳到聊天工具。
4. 用 admin 開啟 `/security`，建立並離線保存「實體復原碼」。
5. 用 admin 開啟 `/config`，只加入隔離的診所 Wi-Fi；重新啟動後確認 AP 關閉。
6. 用 staff 登入，確認只能看到監控、歷史、API 與 CSV；看不到設定、清除、重設、
   憑證或型號控制。再直接請求管理者端點，確認回應被拒絕。

## 血壓計與驗收

1. HBP-9030 自行接 AC 電源。
2. 以符合 USB 2.0 的 Type-A-to-Type-B 線連接參考 host port 與後方 USB2。
3. 在 HBP-9030 功能選擇模式將項目 32 設成 5。
4. 確認 Web 顯示支援協定、transport 就緒、firmware version/build SHA、data loss
   與 reconnect count。
5. 取得一筆量測，確認 freshness 是 `current`、設備時間來源為 `device`，並顯示
   quality/movement 與不識別 record/session sequence。
6. 至少間隔 1 分鐘再量一次；兩筆需分別保存，不得由裝置自動平均。
7. 重新啟動；開機前保存的最新值必須標示 `historical`，不能顯示成 current。
8. 依 [`hardware.md`](hardware.md#device-in-loop-checklist) 完成拔插、ring wrap、
   stale、權限、行動版與 soak 檢查並保存證據。

## 操作狀態

- `current`：本次開機已成功保存，且未超過設定 stale interval。
- `stale`：本次開機量測已超過 interval；數值仍可看，但必須重新量測。
- `historical`：從 NVS 載入的開機前資料，沒有本次接收時間證據。
- `disconnected`：本次量測後資料通道中斷；畫面只顯示最後量測。
- `invalid`：沒有可用量測或量測未通過驗證。

API 的 `revision` 使用持久化 `uint64_t record_sequence`，ring 滿後仍會增加。revision
改變時首頁整體更新 KPI、最近歷史與去識別化 diagnostic state。輪詢失敗會顯示
中斷與最後成功更新時間，不會靜默保留看似即時的畫面。

## 保存、匯出與隱私

- 裝置最多持久保存 20 筆；新記錄覆蓋最舊記錄，沒有日期式自動到期。
- 裝置不接受病患 ID，也不保存、呈現、匯出或記錄 HBP-9030 subject ID/raw frame。
- record/session sequence 只是交接 token；病患關聯只能在受信任的外部診所系統完成。
- 診所資料負責人需決定匯出週期、外部保存期限與清除時點。CSV 仍屬量測資料，需
  存到受控位置。
- 清除後的 NVS 邏輯刪除不等於鑑識級抹除；退役限制見 [`security.md`](security.md)。

## 復原與故障處置

- STA 斷線不會自動開 AP。裝置旁長按 Reset 3 秒，才開啟最多 10 分鐘復原 AP。
- 使用離線保存的實體復原碼完成復原；成功後 admin/staff 都會輪替，立即保存新值，
  並再次建立新的實體復原碼。
- storage error 時，不要反覆量測來猜測是否已保存；先查看歷史與診斷，保留畫面及
  serial 事件，交由管理者處理。
- data loss/disconnected 時，先確認 OTG VBUS、Type-B 線與功能項目 32，再拔插並等候
  clean reconnect；不得把中斷前後的 partial frame 當一筆量測。
- 遺失所有 admin、AP 與實體復原碼時，需依受控退役/重新佈署程序處理，不得以共用
  預設秘密繞過安全狀態。

## 參考政策

血壓提示參考 [2025 AHA/ACC 成人高血壓指引](https://professional.heart.org/en/science-news/2025-high-blood-pressure-guideline/top-things-to-know)；
AHA 的量測說明建議每次取得兩筆、至少間隔 1 分鐘，並對高於 180/120 mmHg 的讀值
立即複測與依症狀/診所流程處置。UI 只呈現診所設定的複核提示，不輸出診斷結論。
