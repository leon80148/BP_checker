#ifndef WEB_HANDLER_H
#define WEB_HANDLER_H

#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "BPRecordManager.h"
#include "BP_Parser.h"
#include "CsvExport.h"
#include "WebSecurity.h"

// 處理網頁請求的類
class WebHandler {
private:
  WebServer* server;
  Preferences* preferences;
  BP_RecordManager* recordManager;
  BP_Parser* bpParser;
  String* bp_model;
  String* lastData;
  String* transportName;
  String* transportStatus;
  // 全域 ap_*/hostname 是 const char* 編譯期常數（bp_checker.ino），
  // 用單層 const char* 即可，省一層 indirection
  const char* hostname;
  const char* ap_ssid;
  // 管理密碼快取：所有變更都經由本類（/set_pin）或重啟（GPIO 清除），
  // 快取不會失同步；避免每次頁面渲染/守門都開 NVS
  String adminPin;

  bool isSystolicAbnormal(int value) const {
    return value > 130 || value < 90;
  }

  bool isDiastolicAbnormal(int value) const {
    return value > 80 || value < 50;
  }

  bool isPulseAbnormal(int value) const {
    return value > 100 || value < 60;
  }

  // 回傳 string literal 不配置 String；renderTableValueCell/renderKpiCard 都是 += 用法
  const char* valueClass(bool abnormal) const {
    return abnormal ? "value-bad" : "value-good";
  }

  // 針對使用者可控字串做最小 HTML escape，防止 SSID/型號名含 '<' 把後續解讀成 tag
  String htmlEscape(const String& s) const {
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      switch (c) {
        case '<':  out += "&lt;"; break;
        case '>':  out += "&gt;"; break;
        case '&':  out += "&amp;"; break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default:   out += c;
      }
    }
    return out;
  }

  // 表格中單一欄位：對 invalid record 顯示 "—" 並用中性樣式，避免 -1 紅字。
  // 直接 append 進 out，免每次呼叫都建一個 ~48 byte 的回傳 String 暫物件
  // （20 筆 × 3 欄 × 兩個表格 ≈ 120 個 alloc/render）。
  void renderTableValueCell(String& out, int value, bool valid,
                            bool (WebHandler::*abnormalFn)(int) const) const {
    bool ok = valid && value > 0;
    if (!ok) { out += "<td class='value-na'>—</td>"; return; }
    bool bad = (this->*abnormalFn)(value);
    out += "<td class='";
    out += valueClass(bad);
    out += "'>";
    out += value; // int append 用 String 內建 overload，免 String(value) 暫物件
    out += "</td>";
  }

  // KPI 卡片：valueOk=false 顯示 "—" + 中性 pill，避免 -1 之類無效數值被誤判為異常。
  // 直接 append 進 out，省每次呼叫一個 ~320B 的中介 String（dashboard 一次 render 3 次）
  void renderKpiCard(String& out, const char* idVal, const char* idPill,
                     const char* label, const char* unit,
                     int value, bool valueOk, bool bad) const {
    out += "<article class='kpi-card'>";
    out += "<div class='kpi-label'><span>";
    out += label;
    out += "</span><span>";
    out += unit;
    out += "</span></div>";
    out += "<div id='";
    out += idVal;
    if (!valueOk) {
      out += "' class='kpi-value value-na'>—</div>";
    } else {
      out += "' class='kpi-value ";
      out += valueClass(bad);
      out += "'>";
      out += value; // 直接 += int，免 String(value) 暫物件
      out += "</div>";
    }
    out += "<span id='";
    out += idPill;
    if (!valueOk) {
      out += "' class='state-pill state-na'>未解析</span>";
    } else {
      out += "' class='state-pill ";
      out += bad ? "state-alert" : "state-ok";
      out += "'>";
      out += bad ? "異常" : "正常";
      out += "</span>";
    }
    out += "</article>";
  }

  // 直接 append 進 out，省每次呼叫一個 ~80B 的中介 String（buildPageStart 4 次 / 渲染）
  void navLink(String& out, const String& href, const String& label, const String& activePath) const {
    out += "<a class='top-nav-link";
    if (href == activePath) out += " active";
    out += "' href='";
    out += href;
    out += "'>";
    out += label;
    out += "</a>";
  }

  const String& sharedStyle() const {
    static String css; // 函式 local 靜態，與 instance const 性無關
    if (!css.isEmpty()) return css;
    css.reserve(5200); // 量測實際 CSS ~5025 bytes，4500 會觸發一次 realloc
    css += "<style>";
    css += ":root{";
    css += "--bg:#edf3fb;";
    css += "--surface:#ffffff;";
    css += "--surface-2:#f7faff;";
    css += "--text:#12243c;";
    css += "--muted:#60708a;";
    css += "--primary:#0f62fe;";
    css += "--primary-ink:#ffffff;";
    css += "--primary-soft:#dce8ff;";
    css += "--success:#118a4c;";
    css += "--danger:#d93025;";
    css += "--warning:#f59e0b;";
    css += "--border:#d6e2f1;";
    css += "--shadow:0 14px 32px rgba(11,35,74,.10);";
    css += "}";
    css += "*{box-sizing:border-box;}";
    css += "body{margin:0;font-family:'Avenir Next','Segoe UI','Noto Sans TC',Arial,sans-serif;background:linear-gradient(180deg,#eaf1fb 0%,#f8fbff 45%,#eef5ff 100%);color:var(--text);} ";
    css += ".app-shell{max-width:1120px;margin:0 auto;padding:24px 18px 48px;}";
    css += ".header-bar{display:flex;justify-content:space-between;align-items:flex-end;gap:16px;margin-bottom:14px;flex-wrap:wrap;}";
    css += ".page-title{margin:0;font-size:30px;line-height:1.1;letter-spacing:.3px;}";
    css += ".chip{display:inline-flex;align-items:center;gap:6px;padding:7px 12px;border-radius:999px;background:rgba(15,98,254,.12);color:#0b4dd0;font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.08em;}";

    css += ".top-nav{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:18px;}";
    css += ".top-nav-link{padding:10px 14px;border-radius:12px;text-decoration:none;color:#1f3558;background:rgba(255,255,255,.68);border:1px solid var(--border);font-weight:700;font-size:14px;}";
    css += ".top-nav-link.active{background:var(--primary);color:var(--primary-ink);border-color:var(--primary);box-shadow:0 8px 18px rgba(15,98,254,.28);} ";

    css += ".panel{background:var(--surface);border:1px solid var(--border);border-radius:18px;padding:18px 20px;margin-bottom:16px;box-shadow:var(--shadow);} ";
    css += ".panel h2{margin:0 0 10px;font-size:22px;}";
    css += ".panel h3{margin:0 0 8px;font-size:18px;}";
    css += ".section-head{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:12px;}";
    css += ".helper-text{color:var(--muted);margin:0 0 10px;line-height:1.6;font-size:14px;}";
    css += ".last-updated{font-size:12px;font-weight:700;color:#35578c;background:var(--primary-soft);padding:6px 10px;border-radius:999px;}";

    css += ".btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:10px 14px;border-radius:12px;background:var(--primary);color:var(--primary-ink);text-decoration:none;border:none;cursor:pointer;font-size:14px;font-weight:700;}";
    css += ".btn:hover{opacity:.94;}";
    css += ".btn-secondary{background:#0b7fab;}";
    css += ".btn-ghost{background:var(--surface-2);color:#1f3558;border:1px solid var(--border);}";
    css += ".btn-danger{background:var(--danger);color:#fff;}";

    css += ".latest-vitals{position:relative;overflow:hidden;}";
    css += ".kpi-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;}";
    css += ".kpi-card{background:linear-gradient(180deg,#ffffff,#f5f9ff);border:1px solid var(--border);border-radius:14px;padding:14px;min-height:132px;}";
    css += ".kpi-label{font-size:13px;color:var(--muted);font-weight:700;display:flex;justify-content:space-between;align-items:center;gap:8px;}";
    css += ".kpi-value{font-size:40px;line-height:1;font-weight:800;margin:10px 0 8px;}";
    css += ".value-good{color:var(--success);}";
    css += ".value-bad{color:var(--danger);}";
    css += ".value-na{color:var(--muted);}";
    css += ".state-pill{display:inline-block;padding:4px 8px;border-radius:999px;font-size:12px;font-weight:700;}";
    css += ".state-ok{background:rgba(17,138,76,.12);color:var(--success);}";
    css += ".state-alert{background:rgba(217,48,37,.12);color:var(--danger);}";
    css += ".state-na{background:rgba(96,112,138,.14);color:var(--muted);}";

    css += ".recent-table table,.history-table table{width:100%;border-collapse:collapse;font-size:14px;overflow:hidden;border-radius:12px;}";
    css += ".recent-table th,.recent-table td,.history-table th,.history-table td{padding:11px 10px;border-bottom:1px solid #e5edf7;text-align:center;}";
    css += ".recent-table th,.history-table th{background:#eff5ff;font-size:12px;letter-spacing:.06em;text-transform:uppercase;color:#4d6282;}";
    css += ".recent-table tr:nth-child(even),.history-table tr:nth-child(even){background:#fbfdff;}";

    css += ".status-list{list-style:none;margin:0;padding:0;display:grid;gap:8px;}";
    css += ".status-list li{display:flex;justify-content:space-between;gap:12px;padding:8px 10px;border-radius:10px;background:#f7faff;border:1px solid #e7eef9;}";

    css += ".raw-data summary{cursor:pointer;font-weight:700;outline:none;}";
    css += ".raw-data[open] summary{margin-bottom:12px;}";
    css += ".raw-data pre,.raw-data .data-section pre{background:#0f172a;color:#dbeafe;padding:14px;border-radius:10px;overflow-x:auto;font-size:12px;}";

    css += ".form-shell{max-width:760px;}";
    css += ".form-shell form{display:grid;gap:10px;}";
    css += ".field-label{font-size:14px;font-weight:800;color:#223a5f;}";
    css += "input,select{width:100%;padding:10px 12px;border:1px solid #c8d7ea;border-radius:10px;background:#fff;font-size:15px;}";
    css += ".scan-refresh{white-space:nowrap;}";

    css += ".inline-actions{display:flex;gap:10px;flex-wrap:wrap;align-items:center;}";
    css += ".danger-zone{border-color:rgba(217,48,37,.3);background:linear-gradient(180deg,#fff,#fff6f6);}";
    css += ".danger-zone .helper-text{color:#7a3f3c;}";
    css += ".text-link{color:#0b54e2;text-decoration:none;font-weight:700;}";

    css += "@media (max-width:980px){.kpi-grid{grid-template-columns:1fr;} .page-title{font-size:26px;}}";
    css += "@media (max-width:640px){.app-shell{padding:18px 12px 32px;} .panel{padding:14px;} .top-nav-link,.btn{width:100%;justify-content:center;} .inline-actions{display:grid;} .kpi-value{font-size:34px;} .status-list li{display:block;} }";
    css += "</style>";
    return css;
  }

  String buildPageStart(const String& title, const String& activePath, bool autoRefresh = false, const String& extraHead = "") const {
    String html;
    // CSS ~5.2KB + head/nav 樣板 ~700B + 留一點給小頁面 body 共 ~2KB ≈ 8KB
    // 大頁面（handleMonitor/handleHistory）會在自己的 handler 再 reserve 更多
    html.reserve(8192);
    html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<title>" + title + "</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    if (autoRefresh) {
      html += "<meta http-equiv='refresh' content='3'>";
    }
    html += sharedStyle();
    html += extraHead;
    html += "</head><body>";
    html += "<div class='app-shell'>";
    html += "<header class='header-bar'>";
    html += "<h1 class='page-title'>" + title + "</h1>";
    html += "<span class='chip'>Health Monitor</span>";
    html += "</header>";
    html += "<nav class='top-nav'>";
    navLink(html, "/", "監控", activePath);
    navLink(html, "/history", "歷史記錄", activePath);
    navLink(html, "/config", "WiFi 設定", activePath);
    navLink(html, "/bp_model", "型號設定", activePath);
    html += "</nav>";
    return html;
  }

  // 回 const char*：caller 用 `html += buildPageEnd()` 是 String += const char*，
  // 不會建臨時 String（每頁 render 省一個 ~30 byte alloc）
  const char* buildPageEnd() const {
    return "</div></body></html>";
  }

public:
  WebHandler(WebServer* server, Preferences* preferences, BP_RecordManager* recordManager,
             BP_Parser* bpParser,
             String* bp_model, String* lastData, String* transportName, String* transportStatus,
             const char* hostname, const char* ap_ssid)
    : server(server),
      preferences(preferences),
      recordManager(recordManager),
      bpParser(bpParser),
      bp_model(bp_model),
      lastData(lastData),
      transportName(transportName),
      transportStatus(transportStatus),
      hostname(hostname),
      ap_ssid(ap_ssid) {}

  void setupRoutes() {
    // CSRF 檢查需要的 headers（Host 由 WebServer 內建解析，走 hostHeader()）
    static const char* kCollectHeaders[] = {"Origin", "Referer"};
    server->collectHeaders(kCollectHeaders, 2);

    preferences->begin("wifi-config", true);
    adminPin = preferences->getString("admin_pin", "");
    preferences->end();

    server->on("/", HTTP_GET, [this]() { this->handleMonitor(); });
    server->on("/config", HTTP_GET, [this]() { this->handleRoot(); });
    server->on("/configure", HTTP_POST, [this]() { this->handleConfigure(); });
    // /data 回傳最新原始資料的 HTML 片段（含 <div>/<pre> 標籤），不是純文字。
    server->on("/data", HTTP_GET, [this]() {
      server->send(200, "text/html; charset=UTF-8", *lastData);
    });

    // 添加歷史記錄相關API
    server->on("/history", HTTP_GET, [this]() { this->handleHistory(); });
    // CSV 匯出：唯讀，信任等級與 /api/history 一致（LAN 上未認證可讀，
    // 屬既有取捨；含批量量測資料，網路隔離由部署端負責）
    server->on("/export.csv", HTTP_GET, [this]() { this->handleExportCsv(); });
    server->on("/api/history", HTTP_GET, [this]() { this->handleHistoryAPI(); });
    server->on("/api/latest", HTTP_GET, [this]() { this->handleLatestAPI(); });
    // 破壞性操作改為 POST，避免瀏覽器 link prefetch、爬蟲、誤點 GET 觸發。
    server->on("/clear_history", HTTP_POST, [this]() { this->handleClearHistory(); });

    // 添加血壓機型號設定路由
    server->on("/bp_model", HTTP_GET, [this]() { this->handleBpModelPage(); });
    server->on("/set_bp_model", HTTP_POST, [this]() { this->handleSetBpModel(); });

    // 管理密碼設定
    server->on("/set_pin", HTTP_POST, [this]() { this->handleSetPin(); });

    // 添加原始資料顯示路由
    server->on("/raw_data", HTTP_GET, [this]() { this->handleRawData(); });

    server->on("/reset", HTTP_POST, [this]() {
      if (this->csrfBlocked()) return;
      if (this->pinBlocked()) return;
      // 只清 WiFi 相關 keys，保留 bp_model 與 admin_pin（與 UI 標籤一致）
      preferences->begin("wifi-config", false);
      preferences->remove("ssid");
      preferences->remove("password");
      preferences->end();

      String html = this->buildPageStart("重置完成", "/config", false, "<meta http-equiv='refresh' content='3;url=/'>");
      html += "<section class='panel danger-zone'>";
      html += "<h2>WiFi 設定已重置</h2>";
      html += "<p class='helper-text'>裝置將重新啟動並回到 AP 設定模式，請重新連線設定。</p>";
      html += "</section>";
      html += this->buildPageEnd();
      server->send(200, "text/html; charset=UTF-8", html);

      delay(1000);
      ESP.restart();
    });
  }

private:
  // 破壞性/設定變更 POST 的 CSRF 守門：跨源（或 Origin:null / 格式錯誤）
  // 一律 403。純比對邏輯在 WebSecurity.h（host 可測）。
  // 兩層檢查：(1) Host 必須是裝置自身身分（防 DNS rebinding —— 攻擊者
  // 網域解析到裝置 IP 時 Origin 與 Host 一致但都不是裝置）；
  // (2) Origin/Referer 與 Host 同源。
  bool csrfBlocked() {
    String staIp;
    if (WiFi.status() == WL_CONNECTED) staIp = WiFi.localIP().toString();
    String mdnsName(hostname);
    mdnsName += ".local";
    if (!hostIsDevice(server->hostHeader(), WiFi.softAPIP().toString(),
                      staIp, mdnsName)) {
      server->send(403, "text/plain; charset=UTF-8", "無效的主機名稱");
      return true;
    }
    if (csrfCheckPasses(server->header("Origin"), server->header("Referer"),
                        server->hostHeader())) {
      return false;
    }
    server->send(403, "text/plain; charset=UTF-8", "跨來源請求被拒絕");
    return true;
  }

  // 管理密碼守門：未設定 PIN 時放行（向後相容）；設定後破壞性/設定變更
  // POST 必須附正確的 pin 欄位。失敗加短延遲抑制暴力嘗試（不做鎖定計數
  // 器 —— 會變成 DoS 途徑）。
  bool pinBlocked() {
    if (pinCheckPasses(server->arg("pin"), adminPin)) return false;
    delay(500);
    server->send(403, "text/plain; charset=UTF-8", "管理密碼錯誤");
    return true;
  }

  // 已設定管理密碼時，在受保護表單內渲染 PIN 輸入欄
  void renderPinField(String& out) const {
    if (adminPin.length() == 0) return;
    out += "<label class='field-label'>管理密碼</label>";
    out += "<input type='password' name='pin' placeholder='輸入管理密碼' autocomplete='current-password' required>";
  }

  // 以下 handle* 都只在 setupRoutes 內以 lambda 連接到 route，無外部 caller。
  void handleRoot() {
    // 同步版 WiFi.scanNetworks() 會阻塞 loop ~2-5 秒，此時血壓機若送資料
    // 可能在 driver buffer 溢出。改 async：頁面立即用上次完成的掃描結果
    // 渲染，掃描在背景進行，完成後下次載入頁面時收割進快取。
    static String cachedOptions; // 上次完成掃描的 <option> 清單
    static bool scanEverDone = false;

    int scanState = WiFi.scanComplete();
    if (scanState >= 0) {
      // 掃描完成：複製進自己的快取後立即 scanDelete 釋放結果記憶體
      cachedOptions = "";
      for (int i = 0; i < scanState; ++i) {
        int quality = 2 * (WiFi.RSSI(i) + 100);
        if (quality > 100) quality = 100;
        if (quality < 0) quality = 0;

        // 鄰近 AP 廣播的 SSID 屬於外部輸入，可能含 ' " < 等字元；escape 後寫入
        String safeSsid = htmlEscape(WiFi.SSID(i));
        cachedOptions += "<option value='" + safeSsid + "'>";
        cachedOptions += safeSsid + " (" + quality + "%";
        if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
          cachedOptions += " 開放";
        }
        cachedOptions += ")</option>";
      }
      WiFi.scanDelete();
      scanEverDone = true;
      scanState = WIFI_SCAN_FAILED; // 收割後回到 idle 狀態
    }

    bool scanRunning = (scanState == WIFI_SCAN_RUNNING);
    bool forceRescan = (server->arg("rescan") == "1");
    // 只在使用者要求或還沒有任何結果時才啟動掃描；掃描進行中不重複啟動
    if (!scanRunning && (forceRescan || !scanEverDone)) {
      WiFi.scanNetworks(true); // async，結果由之後的請求收割
      scanRunning = true;
    }

    // 編譯期合併字面量（adjacent string literal concatenation）一次配置 String
    String js =
      "<script>"
      "function toggleManualSSID(){"
        "var select=document.getElementById('wifi-select');"
        "var manualInput=document.getElementById('manual-ssid');"
        "if(select.value==='manual'){manualInput.style.display='block';manualInput.required=true;}"
        "else{manualInput.style.display='none';manualInput.required=false;}"
      "}"
      "</script>";

    String html = buildPageStart("WiFi 設定", "/config", false, js);
    html.reserve(8192); // reserve 必須在 buildPageStart 之後；含 CSS + scan 結果 dropdown
    html += "<section class='panel form-shell'>";
    html += "<h2>網路連線設定</h2>";
    html += "<p class='helper-text'>選擇可用 WiFi，或使用手動輸入 SSID。儲存後裝置會自動重啟並嘗試連線。</p>";
    html += "<form method='post' action='/configure'>";

    html += "<label class='field-label' for='wifi-select'>WiFi 網路</label>";
    html += "<select id='wifi-select' name='ssid' onchange='toggleManualSSID()'>";

    if (cachedOptions.length() > 0) {
      html += cachedOptions;
    } else if (scanEverDone) {
      html += "<option value=''>找不到 WiFi 網路</option>";
    } else {
      html += "<option value=''>掃描中...</option>";
    }

    html += "<option value='manual'>手動輸入...</option>";
    html += "</select>";
    if (scanRunning) {
      html += "<p class='helper-text'>背景掃描中，稍後重新整理頁面可取得最新清單。</p>";
    }
    html += "<p class='helper-text'>若掃描列表中沒有目標網路，請改用手動輸入。</p>";

    html += "<input type='text' id='manual-ssid' name='manual_ssid' placeholder='輸入 WiFi 名稱' style='display:none'>";

    html += "<label class='field-label' for='wifi-password'>WiFi 密碼</label>";
    html += "<input id='wifi-password' type='password' name='password' placeholder='輸入 WiFi 密碼'>";

    renderPinField(html);

    html += "<div class='inline-actions'>";
    html += "<button class='btn' type='submit'>儲存並連接</button>";
    html += "<a class='btn btn-secondary scan-refresh' href='/config?rescan=1'>重新掃描 WiFi</a>";
    html += "</div>";
    html += "</form>";

    if (WiFi.status() == WL_CONNECTED) {
      html += "<div class='panel' style='margin:14px 0 0;padding:14px 16px;'>";
      html += "<h3>目前連線狀態</h3>";
      html += "<ul class='status-list'>";
      html += "<li><span>狀態</span><strong>已連接</strong></li>";
      html += "<li><span>IP 位址</span><strong>";
      html += WiFi.localIP().toString();
      html += "</strong></li>";
      html += "<li><span>瀏覽器存取</span><strong>http://";
      html += hostname;
      html += ".local</strong></li>";
      html += "</ul></div>";
    }

    html += "</section>";

    // 管理密碼設定：設定後所有設定變更/破壞性操作都需要 PIN。
    // 注意：未設定期間任何連上網頁的人都能先設走 —— 首次部署時就該設定；
    // 忘記密碼的救援路徑是長按裝置 Reset 鈕 3 秒（一併清除 PIN）。
    html += "<section class='panel form-shell'>";
    html += "<h2>管理密碼</h2>";
    html += "<p class='helper-text'>設定後，WiFi 設定、型號切換、清除記錄與重置操作都需輸入此密碼。忘記密碼可長按裝置 Reset 鈕 3 秒清除。建議部署時立即設定。</p>";
    html += "<form method='post' action='/set_pin'>";
    if (adminPin.length() > 0) {
      html += "<label class='field-label'>目前密碼</label>";
      html += "<input type='password' name='current_pin' placeholder='輸入目前管理密碼' autocomplete='current-password' required>";
    }
    html += "<label class='field-label'>新密碼（4-16 字元，留空 = 移除密碼）</label>";
    html += "<input type='password' name='new_pin' placeholder='輸入新管理密碼' autocomplete='new-password'>";
    html += "<button class='btn' type='submit'>儲存管理密碼</button>";
    html += "</form>";
    html += "</section>";

    html += buildPageEnd();

    server->send(200, "text/html; charset=UTF-8", html);
  }

  void handleConfigure() {
    if (csrfBlocked()) return;
    if (pinBlocked()) return;
    String new_ssid;
    String new_password = server->arg("password");

    if (server->arg("ssid") == "manual") {
      new_ssid = server->arg("manual_ssid");
    } else {
      new_ssid = server->arg("ssid");
    }
    new_ssid.trim();

    // 802.11 SSID 上限 32 bytes；WPA2 password 上限 63 chars（open AP 可空）。
    // 拒絕超出規格的輸入，避免存到 NVS 後依然連不上。
    if (new_ssid.length() == 0 || new_ssid.length() > 32) {
      server->send(400, "text/plain; charset=UTF-8", "無效的WiFi設定（SSID 長度需 1-32）");
      return;
    }
    if (new_password.length() > 63) {
      server->send(400, "text/plain; charset=UTF-8", "無效的WiFi設定（密碼長度需 0-63）");
      return;
    }

    preferences->begin("wifi-config", false);
    preferences->putString("ssid", new_ssid);
    preferences->putString("password", new_password);
    preferences->end();

    String html = buildPageStart("設定完成", "/config");
    html += "<section class='panel'>";
    html += "<h2>WiFi 設定已儲存</h2>";
    html += "<p class='helper-text'>設備將重新啟動並嘗試連接新 WiFi。若失敗，可長按 Reset 3 秒還原設定。</p>";
    html += "<ul class='status-list'>";
    html += "<li><span>目標 SSID</span><strong>";
    html += htmlEscape(new_ssid);
    html += "</strong></li>";
    html += "<li><span>設備網址</span><strong>http://";
    html += hostname;
    html += ".local</strong></li>";
    html += "<li><span>備援 AP</span><strong>";
    html += ap_ssid;
    html += "</strong></li>";
    html += "</ul>";
    html += "</section>";
    html += buildPageEnd();

    server->send(200, "text/html; charset=UTF-8", html);

    delay(2000);
    ESP.restart();
  }

  void handleBpModelPage() {
    String html = buildPageStart("血壓機型號設定", "/bp_model");
    html += "<section class='panel form-shell'>";
    html += "<h2>型號設定</h2>";
    html += "<p class='helper-text'>目前內建 OMRON-HBP9030，若接入其他格式可選擇自定義。</p>";
    html += "<form method='post' action='/set_bp_model'>";
    html += "<label class='field-label' for='model-select'>選擇血壓機型號</label>";
    html += "<select id='model-select' name='model'>";
    html += "<option value='OMRON-HBP9030'";
    if (*bp_model == "OMRON-HBP9030") html += " selected";
    html += ">OMRON HBP-9030</option>";
    html += "<option value='CUSTOM'";
    if (*bp_model == "CUSTOM") html += " selected";
    html += ">自定義格式</option>";
    html += "</select>";
    renderPinField(html);
    html += "<button class='btn' type='submit'>儲存設定</button>";
    html += "</form>";
    html += "<div class='panel' style='margin:14px 0 0;padding:14px 16px;'>";
    html += "<h3>目前型號</h3>";
    html += "<p class='helper-text'>" + htmlEscape(*bp_model) + "</p>";
    html += "</div>";
    html += "</section>";
    html += buildPageEnd();

    server->send(200, "text/html; charset=UTF-8", html);
  }

  void handleSetBpModel() {
    if (csrfBlocked()) return;
    if (pinBlocked()) return; // 選錯型號會靜默破壞解析，不算無害操作
    String new_model = server->arg("model");

    if (new_model.length() > 0) {
      preferences->begin("wifi-config", false);
      preferences->putString("bp_model", new_model);
      preferences->end();
      *bp_model = new_model;
      // 之前漏了這行：parser 留在舊型號，UI 切換完全不會生效到下次重啟前
      bpParser->setModel(new_model);

      String html = buildPageStart("型號設定完成", "/bp_model", false, "<meta http-equiv='refresh' content='2;url=/'>");
      html += "<section class='panel'>";
      html += "<h2>已套用型號：" + htmlEscape(new_model) + "</h2>";
      html += "<p class='helper-text'>系統將返回監控頁面。</p>";
      html += "</section>";
      html += buildPageEnd();

      server->send(200, "text/html; charset=UTF-8", html);
    } else {
      server->send(400, "text/plain; charset=UTF-8", "無效的型號設定");
    }
  }

  void handleMonitor() {
    // 不再用 meta refresh：JS 每 3 秒 fetch /api/latest 只更新數值節點，
    // 替代每次重建 ~10KB 整頁 HTML。
    String html = buildPageStart("血壓監控儀表板", "/", false);
    // reserve 必須在 assignment 之後（buildPageStart 回傳的 String 透過 move-assignment
    // 會把 html 容量重設成它自己的 ~6KB）。完整 dashboard ~11.5KB worst case。
    html.reserve(11264);

    int recordCount = recordManager->getRecordCount();
    if (recordCount > 0) {
      const BPData& latest = recordManager->getLatestRecord();

      // 無效記錄（valid=false 或數值非正）一律顯示中性 "—"，避免 "-1 異常" 紅字誤導
      bool sysOk = latest.valid && latest.systolic > 0;
      bool diaOk = latest.valid && latest.diastolic > 0;
      bool pulOk = latest.valid && latest.pulse > 0;
      bool sysBad = sysOk && isSystolicAbnormal(latest.systolic);
      bool diaBad = diaOk && isDiastolicAbnormal(latest.diastolic);
      bool pulBad = pulOk && isPulseAbnormal(latest.pulse);

      html += "<section class='panel latest-vitals'>";
      html += "<div class='section-head'><h2>最新量測</h2>";
      html += "<span id='last-updated' class='last-updated'>最後更新：";
      html += latest.timestamp;
      html += "（每 3 秒刷新）</span></div>";
      html += "<div class='kpi-grid'>";

      renderKpiCard(html, "kpi-sys", "pill-sys", "收縮壓", "mmHg", latest.systolic, sysOk, sysBad);
      renderKpiCard(html, "kpi-dia", "pill-dia", "舒張壓", "mmHg", latest.diastolic, diaOk, diaBad);
      renderKpiCard(html, "kpi-pul", "pill-pul", "脈搏",   "bpm",  latest.pulse,    pulOk, pulBad);

      html += "</div></section>";

      html += "<section class='panel recent-table'>";
      html += "<div class='section-head'>";
      html += "<h2>最近 5 筆數據</h2>";
      html += "<a class='btn btn-ghost' href='/history'>查看完整歷史</a>";
      html += "</div>";
      html += "<table>";
      html += "<tr><th>測量時間</th><th>收縮壓 (mmHg)</th><th>舒張壓 (mmHg)</th><th>脈搏 (bpm)</th></tr>";

      int displayCount = min(5, recordCount);
      for (int i = 0; i < displayCount; i++) {
        const BPData& record = recordManager->getRecord(i);
        html += "<tr><td>";
        html += record.timestamp;
        html += "</td>";
        renderTableValueCell(html, record.systolic, record.valid, &WebHandler::isSystolicAbnormal);
        renderTableValueCell(html, record.diastolic, record.valid, &WebHandler::isDiastolicAbnormal);
        renderTableValueCell(html, record.pulse, record.valid, &WebHandler::isPulseAbnormal);
        html += "</tr>";
      }

      html += "</table></section>";
    } else {
      html += "<section class='panel latest-vitals'>";
      html += "<h2>最新量測</h2>";
      html += "<p class='helper-text'>尚未收到血壓數據。請先確認目前資料通道狀態，再檢查血壓機連線。</p>";
      html += "<span class='last-updated'>每 3 秒自動刷新</span>";
      html += "</section>";
    }

    html += "<details class='panel raw-data'>";
    html += "<summary>原始資料</summary>";
    if (*lastData == "") {
      html += "<p class='helper-text'>等待數據...</p>";
    } else {
      html += *lastData;
    }
    html += "</details>";

    // ternary 兩端需 common type，會多生一個 String temp；改 if/else 直接 assign
    String wifiIp;
    if (WiFi.status() == WL_CONNECTED) wifiIp = WiFi.localIP().toString();
    else wifiIp = "未連線";
    html += "<section class='panel'>";
    html += "<h2>連線資訊</h2>";
    html += "<ul class='status-list'>";
    html += "<li><span>設備名稱</span><strong>BP_checker</strong></li>";
    html += "<li><span>血壓機型號</span><strong>";
    html += htmlEscape(*bp_model);
    html += "</strong></li>";
    html += "<li><span>資料通道</span><strong id='conn-transport'>";
    html += *transportName;
    html += "</strong></li>";
    html += "<li><span>通道狀態</span><strong id='conn-status'>";
    html += *transportStatus;
    html += "</strong></li>";
    html += "<li><span>WiFi IP</span><strong id='conn-ip'>";
    html += wifiIp;
    html += "</strong></li>";
    html += "<li><span>可訪問網址</span><strong>http://";
    html += hostname; // const char* via String += overload, no temporary
    html += ".local</strong></li>";
    // AP 密碼不顯示在頁面上（任何連上網頁的人都看得到 dashboard）
    html += "<li><span>AP 熱點</span><strong>";
    html += ap_ssid;
    html += "</strong></li>";
    html += "</ul>";
    html += "</section>";

    html += "<section class='panel danger-zone'>";
    html += "<h3>維護操作</h3>";
    html += "<p class='helper-text'>若要重新配網，可重置 WiFi 設定並重啟。</p>";
    html += "<form method='post' action='/reset' onsubmit=\"return confirm('確定要重置 WiFi 設定並重啟嗎？');\" style='display:inline'>";
    renderPinField(html);
    html += "<button type='submit' class='btn btn-danger'>重置 WiFi 設定</button>";
    html += "</form>";
    html += "</section>";

    // AJAX poll：每 3 秒從 /api/latest 取最新狀態並就地更新 DOM。
    // 比起原 meta refresh，伺服器端輸出量從 ~10KB 降到 ~300B，heap churn 大減。
    html += F(
      "<script>"
      "async function bpRefresh(){"
        "try{"
        // 切走 tab 時 browser 會 throttle 但仍 fire；明確 skip 省 device 處理
        "if(document.hidden)return;"
        "const r=await fetch('/api/latest');if(!r.ok)return;"
        "const d=await r.json();"
        "const t=document.getElementById('conn-transport');if(t)t.textContent=d.transport_name;"
        "const s=document.getElementById('conn-status');if(s)s.textContent=d.transport_status;"
        "const ip=document.getElementById('conn-ip');if(ip)ip.textContent=d.wifi_ip||'未連線';"
        "if(d.count>0){"
          "if(!document.getElementById('kpi-sys')){location.reload();return;}"
          "const v=d.valid===true;"
          "bpKpi('kpi-sys','pill-sys',d.systolic,d.sysBad,v&&d.systolic>0);"
          "bpKpi('kpi-dia','pill-dia',d.diastolic,d.diaBad,v&&d.diastolic>0);"
          "bpKpi('kpi-pul','pill-pul',d.pulse,d.pulBad,v&&d.pulse>0);"
          "const u=document.getElementById('last-updated');"
          "if(u)u.textContent='最後更新：'+d.timestamp+'（每 3 秒刷新）';"
        "}else if(document.getElementById('kpi-sys')){"
          "location.reload();return;"
        "}}catch(e){}"
        // 用 setTimeout chain 取代 setInterval：確保前一次 poll 完成（含網路慢/錯誤）
        // 後才排下一次，避免設備重啟或卡頓時 request 堆疊
        "finally{setTimeout(bpRefresh,3000);}"
      "}"
      "function bpKpi(iv,ip,v,bad,ok){"
        "const a=document.getElementById(iv),b=document.getElementById(ip);"
        "if(!ok){"
          "if(a){a.textContent='—';a.className='kpi-value value-na';}"
          "if(b){b.textContent='未解析';b.className='state-pill state-na';}"
          "return;"
        "}"
        "if(a){a.textContent=v;a.className='kpi-value '+(bad?'value-bad':'value-good');}"
        "if(b){b.textContent=bad?'異常':'正常';b.className='state-pill '+(bad?'state-alert':'state-ok');}"
      "}"
      "bpRefresh();"
      "</script>"
    );

    html += buildPageEnd();
    server->send(200, "text/html; charset=UTF-8", html);
  }

  void handleHistory() {
    String html = buildPageStart("血壓歷史記錄", "/history");
    // reserve 必須在 buildPageStart 回傳之後（move-assignment 會把容量重設）。
    // CSS ~4.5KB + nav/header ~700B + 20 列表（~250B/row × 20 = 5KB）+ danger zone ~300B
    html.reserve(12288);

    html += "<section class='panel history-table'>";
    html += "<div class='section-head'>";
    html += "<h2>所有歷史數據</h2>";
    html += "<div class='inline-actions'>";
    html += "<a href='/export.csv' class='btn btn-secondary'>匯出 CSV</a>";
    html += "<a href='/' class='btn btn-ghost'>返回監控</a>";
    html += "</div>";
    html += "</div>";

    html += "<table>";
    html += "<tr><th>測量時間</th><th>收縮壓 (mmHg)</th><th>舒張壓 (mmHg)</th><th>脈搏 (bpm)</th><th>原始數據</th></tr>";

    int recordCount = recordManager->getRecordCount();
    if (recordCount > 0) {
      for (int i = 0; i < recordCount; i++) {
        const BPData& record = recordManager->getRecord(i);
        html += "<tr><td>";
        html += record.timestamp;
        html += "</td>";
        renderTableValueCell(html, record.systolic, record.valid, &WebHandler::isSystolicAbnormal);
        renderTableValueCell(html, record.diastolic, record.valid, &WebHandler::isDiastolicAbnormal);
        renderTableValueCell(html, record.pulse, record.valid, &WebHandler::isPulseAbnormal);
        // rawData 僅存於 RAM，重啟後從 NVS 載入的記錄沒有原始位元組；
        // 沒資料就顯示 dash，避免使用者點進去看到空白頁
        if (record.rawData.length() > 0) {
          html += "<td><a href='/raw_data?id=";
          html += i;
          html += "' class='text-link'>查看原始數據</a></td></tr>";
        } else {
          html += "<td class='value-na'>—</td></tr>";
        }
      }
    } else {
      html += "<tr><td colspan='5'>尚無歷史記錄</td></tr>";
    }

    html += "</table>";
    html += "</section>";

    html += "<section class='panel danger-zone'>";
    html += "<h3>危險操作</h3>";
    html += "<p class='helper-text'>此操作會清除全部歷史資料且無法復原。</p>";
    html += "<form method='post' action='/clear_history' onsubmit=\"return confirm('確定要清除所有歷史記錄嗎？');\" style='display:inline'>";
    renderPinField(html);
    html += "<button type='submit' class='btn btn-danger'>清除記錄</button>";
    html += "</form>";
    html += "</section>";

    html += buildPageEnd();
    server->send(200, "text/html; charset=UTF-8", html);
  }

  void handleHistoryAPI() {
    // ArduinoJson v6：傳 String 進去會 copy 進 doc pool；改用 c_str() 直接引用，
    // 在 single-thread handler 內 BPData 不會被修改，pointer 安全。
    // 20 筆 × JSON_OBJECT_SIZE(5) + JSON_ARRAY_SIZE(20) ≈ 1936B，2048 足夠且省 1KB loopTask stack。
    StaticJsonDocument<2048> doc;
    JsonArray records = doc.createNestedArray("records");

    int recordCount = recordManager->getRecordCount();
    for (int i = 0; i < recordCount; i++) {
      const BPData& record = recordManager->getRecord(i);

      JsonObject recordObj = records.createNestedObject();
      recordObj["timestamp"] = record.timestamp.c_str();
      recordObj["systolic"] = record.systolic;
      recordObj["diastolic"] = record.diastolic;
      recordObj["pulse"] = record.pulse;
      recordObj["valid"] = record.valid;
    }

    String jsonStr;
    serializeJson(doc, jsonStr);
    server->send(200, "application/json", jsonStr);
  }

  // 給 dashboard JS 輪詢用的小型狀態端點，~300 bytes
  void handleLatestAPI() {
    StaticJsonDocument<512> doc;
    int count = recordManager->getRecordCount();
    doc["count"] = count;
    if (count > 0) {
      const BPData& latest = recordManager->getLatestRecord();
      doc["timestamp"] = latest.timestamp.c_str();
      doc["systolic"] = latest.systolic;
      doc["diastolic"] = latest.diastolic;
      doc["pulse"] = latest.pulse;
      doc["valid"] = latest.valid;
      doc["sysBad"] = isSystolicAbnormal(latest.systolic);
      doc["diaBad"] = isDiastolicAbnormal(latest.diastolic);
      doc["pulBad"] = isPulseAbnormal(latest.pulse);
    }
    // 整個 request handler 與 syncTransportStatus 都在 main loop 序列執行，
    // 不會發生 mid-request mutation，可直接 c_str() 引用省 pool 複製
    doc["transport_name"] = transportName->c_str();
    doc["transport_status"] = transportStatus->c_str();
    String wifiIp;
    if (WiFi.status() == WL_CONNECTED) wifiIp = WiFi.localIP().toString();
    doc["wifi_ip"] = wifiIp.c_str();

    String jsonStr;
    serializeJson(doc, jsonStr);
    server->send(200, "application/json", jsonStr);
  }

  void handleSetPin() {
    if (csrfBlocked()) return;
    // 已設定 PIN 時，變更/移除都要先驗證目前密碼
    if (!pinCheckPasses(server->arg("current_pin"), adminPin)) {
      delay(500);
      server->send(403, "text/plain; charset=UTF-8", "目前管理密碼錯誤");
      return;
    }
    String newPin = server->arg("new_pin");
    if (newPin.length() > 0 && !isValidPin(newPin)) {
      server->send(400, "text/plain; charset=UTF-8", "無效的管理密碼（需 4-16 個非空白字元）");
      return;
    }

    preferences->begin("wifi-config", false);
    if (newPin.length() == 0) {
      preferences->remove("admin_pin");
    } else {
      preferences->putString("admin_pin", newPin);
    }
    preferences->end();
    adminPin = newPin;

    String html = buildPageStart("管理密碼已更新", "/config", false, "<meta http-equiv='refresh' content='2;url=/config'>");
    html += "<section class='panel'>";
    html += (newPin.length() > 0) ? "<h2>管理密碼已設定</h2>" : "<h2>管理密碼已移除</h2>";
    html += "<p class='helper-text'>正在返回設定頁面...</p>";
    html += "</section>";
    html += buildPageEnd();
    server->send(200, "text/html; charset=UTF-8", html);
  }

  void handleExportCsv() {
    String csv;
    appendHistoryCsv(csv, *recordManager);
    server->sendHeader("Content-Disposition", "attachment; filename=\"bp_history.csv\"");
    server->send(200, "text/csv; charset=UTF-8", csv);
  }

  void handleClearHistory() {
    if (csrfBlocked()) return;
    if (pinBlocked()) return;
    recordManager->clearRecords();
    *lastData = ""; // 同步清掉殘留 raw HTML，避免 dashboard 顯示陳舊資料

    String html = buildPageStart("記錄已清除", "/history", false, "<meta http-equiv='refresh' content='2;url=/history'>");
    html += "<section class='panel danger-zone'>";
    html += "<h2>所有歷史記錄已清除</h2>";
    html += "<p class='helper-text'>正在返回歷史記錄頁面...</p>";
    html += "</section>";
    html += buildPageEnd();

    server->send(200, "text/html; charset=UTF-8", html);
  }

  void handleRawData() {
    // 嚴格解析：toInt() 會把 "abc" 變 0 而誤中 record 0
    int idx = parseIndexParam(server->arg("id"));
    if (idx >= 0) {
      const BPData& record = recordManager->getRecord(idx);
      // 與 handleHistory 的連結條件（rawData.length()>0）一致：
      // invalid 但有原始資料的記錄也要能查看，否則 history 會出現死連結
      if (record.rawData.length() > 0) {
        String html = buildPageStart("原始數據", "/history");
        html += "<section class='panel raw-data'>";
        html += "<div class='section-head'>";
        html += "<h2>量測原始資料</h2>";
        html += "<a href='/history' class='btn btn-ghost'>返回歷史記錄</a>";
        html += "</div>";
        html += record.rawData;
        html += "</section>";
        html += buildPageEnd();

        server->send(200, "text/html; charset=UTF-8", html);
      } else {
        server->send(404, "text/plain; charset=UTF-8", "找不到該記錄");
      }
    } else {
      server->send(400, "text/plain; charset=UTF-8", "缺少記錄ID");
    }
  }
};

#endif
