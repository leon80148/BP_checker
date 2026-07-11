# 安全、隱私與退役邊界

## 資產與威脅模型

需保護的資產包括量測記錄、Wi-Fi 密碼、每台裝置的 AP/bootstrap/admin/staff 秘密、
實體復原碼及裝置可用性。假設攻擊者可能在同網段送出任意 HTTP、猜測 Host/Auth、
重播請求、使連線中斷、切斷 NVS 寫入，或取得退役裝置；不假設同網段內容具機密性。

安全邊界：

- 單一 default-deny route registry 分類匿名、staff、admin 及 request interface。
- 每台裝置以 CSPRNG 產生互相獨立的 128-bit 秘密；失敗時 fail closed。
- claim 只在 provisioning AP 接受一次性 token；復原只在實體啟動、限時 recovery AP。
- staff 只能讀取/匯出去識別量測與 operations state；管理、清除、網路、reset、型號與
  憑證輪替只允許 admin，staff 頁面也不呈現這些控制。
- 記錄 slot、state、clear tombstone 與安全 bundle 都有 crash-consistent 邊界。

## HTTP 與網路殘餘風險

目前 Basic authentication 經 HTTP 傳輸。WPA2 與 VLAN 是部署假設，不是韌體提供的
端到端加密；已在同網段或掌控 AP 的人能看到憑證與量測。只允許診所受管控的隔離
網段，禁止 port forwarding、訪客網路與 Internet 暴露。任何病患識別 payload 都必須
先導入 HTTPS 或受信任 TLS gateway，不能只依賴 Basic auth。

## 資料最小化

- parser 暫時接觸 HBP-9030 subject ID，但接受/拒絕後都清除；它不進入 BPData、NVS、
  HTML、JSON、CSV 或 serial log。
- UI 沒有病患 ID/病歷號欄位。`uint64_t record_sequence` 與 `session_sequence` 是不識別
  token，只能在裝置外的受信任工作流程與病患關聯。
- diagnostic API 只回傳 allowlisted stable state，不回傳 raw frame 或任意輸入。
- quality/movement、timestamp source、freshness 與 operations counters 是必要的安全脈絡，
  用來避免過期或中斷資料偽裝成新量測。

## 憑證、復原與輪替

- 啟用時立即把 admin/staff/AP 與新建實體復原碼存入診所密碼管理器。
- 人員離職、秘密疑似外洩、復原完成或設備轉區時立即輪替相關秘密。
- STA 失聯不會自動開 AP；需裝置旁長按 Reset 3 秒，recovery AP 最多存在 10 分鐘。
- 每次使用實體復原碼後，admin/staff 都會更新，舊碼失效；管理者必須立即另建新碼。
- 不記錄秘密到 ticket、email、聊天、螢幕截圖或一般 serial log 保存系統。

## 保存與清除

預設固定保存最近 20 筆，ring overwrite 最舊記錄；沒有時間式自動保存政策。診所需
決定外部保存與裝置清除時點。admin clear 先提交新 generation tombstone，再清理舊 slot；
中斷後不會回復已清除的舊 generation。

但 NVS remove/覆寫不是鑑識級抹除：未啟用 flash encryption/secure boot 或整個 partition
安全擦除時，具備實體 flash 取證能力者仍可能復原舊頁面。因此：

1. 退役前先匯出必要記錄並執行 decommission reset。
2. 確認網路、歷史、安全秘密與敏感 RAM 清理流程完成。
3. 高風險環境需啟用經驗證的 flash encryption 與 secure boot，或以可稽核方式擦除/銷毀
   儲存媒體；不要把邏輯刪除宣稱為安全抹除。
4. 保存退役人員、裝置識別、時間、firmware SHA 與處理結果，不保存秘密本身。

## 安全事件處置

- 憑證疑似外洩：隔離裝置網段、用可信 admin 路徑輪替；若 admin 失效，實體啟動復原。
- 不明 data loss/reconnect 增長：保存 operations state 與時間，檢查 VBUS/線材，停止把
  畫面當即時資料，完成 clean reconnect 與新量測。
- 儲存錯誤：停止 destructive/repeated 操作，保存診斷，依 crash recovery 程序重啟並
  驗證 history；不可把 handler 回應前的值視為已持久保存。
- 遺失全部復原材料：隔離並走退役/重新 commissioning；不得新增後門或共用密碼。

完整部署步驟見 [`deployment.md`](deployment.md)，硬體驗證見 [`hardware.md`](hardware.md)。
