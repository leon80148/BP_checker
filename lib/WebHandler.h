#ifndef WEB_HANDLER_H
#define WEB_HANDLER_H

#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "BPRecordManager.h"
#include "BP_Parser.h"

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
  const char** hostname;
  const char** ap_ssid;
  const char** ap_password;

  bool isSystolicAbnormal(int value) {
    return value > 130 || value < 90;
  }

  bool isDiastolicAbnormal(int value) {
    return value > 80 || value < 50;
  }

  bool isPulseAbnormal(int value) {
    return value > 100 || value < 60;
  }

  // 回傳 string literal 不配置 String；renderTableValueCell/renderKpiCard 都是 += 用法
  const char* valueClass(bool abnormal) {
    return abnormal ? "value-bad" : "value-good";
  }

  // 針對使用者可控字串做最小 HTML escape，防止 SSID/型號名含 '<' 把後續解讀成 tag
  String htmlEscape(const String& s) {
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

  // 表格中單一欄位：對 invalid record 顯示 "—" 並用中性樣式，避免 -1 紅字
  String renderTableValueCell(int value, bool valid, bool (WebHandler::*abnormalFn)(int)) {
    bool ok = valid && value > 0;
    if (!ok) return "<td class='value-na'>—</td>";
    bool bad = (this->*abnormalFn)(value);
    String s;
    s.reserve(48);
    s += "<td class='";
    s += valueClass(bad);
    s += "'>";
    s += String(value);
    s += "</td>";
    return s;
  }

  // KPI 卡片：valueOk=false 顯示 "—" + 中性 pill，避免 -1 之類無效數值被誤判為異常
  String renderKpiCard(const char* idVal, const char* idPill,
                       const char* label, const char* unit,
                       int value, bool valueOk, bool bad) {
    String html;
    html.reserve(320);
    html += "<article class='kpi-card'>";
    html += "<div class='kpi-label'><span>";
    html += label;
    html += "</span><span>";
    html += unit;
    html += "</span></div>";
    html += "<div id='";
    html += idVal;
    if (!valueOk) {
      html += "' class='kpi-value value-na'>—</div>";
    } else {
      html += "' class='kpi-value ";
      html += valueClass(bad);
      html += "'>";
      html += String(value);
      html += "</div>";
    }
    html += "<span id='";
    html += idPill;
    if (!valueOk) {
      html += "' class='state-pill state-na'>未解析</span>";
    } else {
      html += "' class='state-pill ";
      html += bad ? "state-alert" : "state-ok";
      html += "'>";
      html += bad ? "異常" : "正常";
      html += "</span>";
    }
    html += "</article>";
    return html;
  }

  String navLink(const String& href, const String& label, const String& activePath) {
    String s;
    s.reserve(80);
    s += "<a class='top-nav-link";
    if (href == activePath) s += " active";
    s += "' href='";
    s += href;
    s += "'>";
    s += label;
    s += "</a>";
    return s;
  }

  const String& sharedStyle() {
    static String css;
    if (!css.isEmpty()) return css;
    css.reserve(4500);
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

  String buildPageStart(const String& title, const String& activePath, bool autoRefresh = false, const String& extraHead = "") {
    String html;
    // CSS ~4.5KB + head/nav 樣板 ~700B；預留避免每個 += 觸發 realloc
    html.reserve(6144);
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
    html += navLink("/", "監控", activePath);
    html += navLink("/history", "歷史記錄", activePath);
    html += navLink("/config", "WiFi 設定", activePath);
    html += navLink("/bp_model", "型號設定", activePath);
    html += "</nav>";
    return html;
  }

  String buildPageEnd() {
    return "</div></body></html>";
  }

public:
  WebHandler(WebServer* server, Preferences* preferences, BP_RecordManager* recordManager,
             BP_Parser* bpParser,
             String* bp_model, String* lastData, String* transportName, String* transportStatus,
             const char** hostname, const char** ap_ssid, const char** ap_password) {
    this->server = server;
    this->preferences = preferences;
    this->recordManager = recordManager;
    this->bpParser = bpParser;
    this->bp_model = bp_model;
    this->lastData = lastData;
    this->transportName = transportName;
    this->transportStatus = transportStatus;
    this->hostname = hostname;
    this->ap_ssid = ap_ssid;
    this->ap_password = ap_password;
  }

  void setupRoutes() {
    server->on("/", HTTP_GET, [this]() { this->handleMonitor(); });
    server->on("/config", HTTP_GET, [this]() { this->handleRoot(); });
    server->on("/configure", HTTP_POST, [this]() { this->handleConfigure(); });
    // /data 回傳最新原始資料的 HTML 片段（含 <div>/<pre> 標籤），不是純文字。
    server->on("/data", HTTP_GET, [this]() {
      server->send(200, "text/html; charset=UTF-8", *lastData);
    });

    // 添加歷史記錄相關API
    server->on("/history", HTTP_GET, [this]() { this->handleHistory(); });
    server->on("/api/history", HTTP_GET, [this]() { this->handleHistoryAPI(); });
    server->on("/api/latest", HTTP_GET, [this]() { this->handleLatestAPI(); });
    // 破壞性操作改為 POST，避免瀏覽器 link prefetch、爬蟲、誤點 GET 觸發。
    server->on("/clear_history", HTTP_POST, [this]() { this->handleClearHistory(); });

    // 添加血壓機型號設定路由
    server->on("/bp_model", HTTP_GET, [this]() { this->handleBpModelPage(); });
    server->on("/set_bp_model", HTTP_POST, [this]() { this->handleSetBpModel(); });

    // 添加原始資料顯示路由
    server->on("/raw_data", HTTP_GET, [this]() { this->handleRawData(); });

    server->on("/reset", HTTP_POST, [this]() {
      // 只清 WiFi 相關 keys，保留 bp_model（與 UI 標籤一致）
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
      server->send(200, "text/html", html);

      delay(1000);
      ESP.restart();
    });
  }

  void handleRoot() {
    // WiFi.scanNetworks() 同步阻塞 ~5 秒；快取 20 秒避免每次 /config 都重掃。
    // 顯式加 ?rescan=1 才強制重掃（重新掃描按鈕會帶這參數）。
    static unsigned long lastScanMs = 0;
    static int lastScanCount = -1;
    bool forceRescan = (server->arg("rescan") == "1");
    int n;
    if (forceRescan || lastScanCount < 0 || millis() - lastScanMs > 20000) {
      n = WiFi.scanNetworks();
      lastScanCount = n;
      lastScanMs = millis();
    } else {
      n = lastScanCount;
    }

    String js = "<script>";
    js += "function toggleManualSSID(){";
    js += "var select=document.getElementById('wifi-select');";
    js += "var manualInput=document.getElementById('manual-ssid');";
    js += "if(select.value==='manual'){manualInput.style.display='block';manualInput.required=true;}";
    js += "else{manualInput.style.display='none';manualInput.required=false;}";
    js += "}";
    js += "</script>";

    String html;
    html.reserve(8192); // 含 CSS + scan 結果 dropdown
    html = buildPageStart("WiFi 設定", "/config", false, js);
    html += "<section class='panel form-shell'>";
    html += "<h2>網路連線設定</h2>";
    html += "<p class='helper-text'>選擇可用 WiFi，或使用手動輸入 SSID。儲存後裝置會自動重啟並嘗試連線。</p>";
    html += "<form method='post' action='/configure'>";

    html += "<label class='field-label' for='wifi-select'>WiFi 網路</label>";
    html += "<select id='wifi-select' name='ssid' onchange='toggleManualSSID()'>";

    if (n == 0) {
      html += "<option value=''>找不到 WiFi 網路</option>";
    } else {
      for (int i = 0; i < n; ++i) {
        int quality = 2 * (WiFi.RSSI(i) + 100);
        if (quality > 100) quality = 100;
        if (quality < 0) quality = 0;

        // 鄰近 AP 廣播的 SSID 屬於外部輸入，可能含 ' " < 等字元；escape 後寫入
        String safeSsid = htmlEscape(WiFi.SSID(i));
        html += "<option value='" + safeSsid + "'>";
        html += safeSsid + " (" + quality + "%";
        if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
          html += " 開放";
        }
        html += ")</option>";
      }
    }

    html += "<option value='manual'>手動輸入...</option>";
    html += "</select>";
    html += "<p class='helper-text'>若掃描列表中沒有目標網路，請改用手動輸入。</p>";

    html += "<input type='text' id='manual-ssid' name='manual_ssid' placeholder='輸入 WiFi 名稱' style='display:none'>";

    html += "<label class='field-label' for='wifi-password'>WiFi 密碼</label>";
    html += "<input id='wifi-password' type='password' name='password' placeholder='輸入 WiFi 密碼'>";

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
      html += "<li><span>IP 位址</span><strong>" + WiFi.localIP().toString() + "</strong></li>";
      html += "<li><span>瀏覽器存取</span><strong>http://" + String(*hostname) + ".local</strong></li>";
      html += "</ul></div>";
    }

    html += "</section>";
    html += buildPageEnd();

    server->send(200, "text/html", html);
  }

  void handleConfigure() {
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
      server->send(400, "text/plain", "無效的WiFi設定（SSID 長度需 1-32）");
      return;
    }
    if (new_password.length() > 63) {
      server->send(400, "text/plain", "無效的WiFi設定（密碼長度需 0-63）");
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
    html += "<li><span>目標 SSID</span><strong>" + htmlEscape(new_ssid) + "</strong></li>";
    html += "<li><span>設備網址</span><strong>http://" + String(*hostname) + ".local</strong></li>";
    html += "<li><span>備援 AP</span><strong>" + String(*ap_ssid) + "</strong></li>";
    html += "</ul>";
    html += "</section>";
    html += buildPageEnd();

    server->send(200, "text/html", html);

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
    html += "<button class='btn' type='submit'>儲存設定</button>";
    html += "</form>";
    html += "<div class='panel' style='margin:14px 0 0;padding:14px 16px;'>";
    html += "<h3>目前型號</h3>";
    html += "<p class='helper-text'>" + htmlEscape(*bp_model) + "</p>";
    html += "</div>";
    html += "</section>";
    html += buildPageEnd();

    server->send(200, "text/html", html);
  }

  void handleSetBpModel() {
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

      server->send(200, "text/html", html);
    } else {
      server->send(400, "text/plain", "無效的型號設定");
    }
  }

  void handleMonitor() {
    String html;
    html.reserve(8192); // CSS ~4.5KB + dashboard body ~3KB
    // 不再用 meta refresh：JS 每 3 秒 fetch /api/latest 只更新數值節點，
    // 替代每次重建 ~10KB 整頁 HTML。
    html = buildPageStart("血壓監控儀表板", "/", false);

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

      html += renderKpiCard("kpi-sys", "pill-sys", "收縮壓", "mmHg", latest.systolic, sysOk, sysBad);
      html += renderKpiCard("kpi-dia", "pill-dia", "舒張壓", "mmHg", latest.diastolic, diaOk, diaBad);
      html += renderKpiCard("kpi-pul", "pill-pul", "脈搏",   "bpm",  latest.pulse,    pulOk, pulBad);

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
        html += renderTableValueCell(record.systolic, record.valid, &WebHandler::isSystolicAbnormal);
        html += renderTableValueCell(record.diastolic, record.valid, &WebHandler::isDiastolicAbnormal);
        html += renderTableValueCell(record.pulse, record.valid, &WebHandler::isPulseAbnormal);
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

    String wifiIp = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("未連線");
    html += "<section class='panel'>";
    html += "<h2>連線資訊</h2>";
    html += "<ul class='status-list'>";
    html += "<li><span>設備名稱</span><strong>BP_checker</strong></li>";
    html += "<li><span>血壓機型號</span><strong>" + htmlEscape(*bp_model) + "</strong></li>";
    html += "<li><span>資料通道</span><strong id='conn-transport'>" + *transportName + "</strong></li>";
    html += "<li><span>通道狀態</span><strong id='conn-status'>" + *transportStatus + "</strong></li>";
    html += "<li><span>WiFi IP</span><strong id='conn-ip'>" + wifiIp + "</strong></li>";
    html += "<li><span>可訪問網址</span><strong>http://" + String(*hostname) + ".local</strong></li>";
    html += "<li><span>AP 熱點</span><strong>" + String(*ap_ssid) + " (" + String(*ap_password) + ")</strong></li>";
    html += "</ul>";
    html += "</section>";

    html += "<section class='panel danger-zone'>";
    html += "<h3>維護操作</h3>";
    html += "<p class='helper-text'>若要重新配網，可重置 WiFi 設定並重啟。</p>";
    html += "<form method='post' action='/reset' onsubmit=\"return confirm('確定要重置 WiFi 設定並重啟嗎？');\" style='display:inline'>";
    html += "<button type='submit' class='btn btn-danger'>重置 WiFi 設定</button>";
    html += "</form>";
    html += "</section>";

    // AJAX poll：每 3 秒從 /api/latest 取最新狀態並就地更新 DOM。
    // 比起原 meta refresh，伺服器端輸出量從 ~10KB 降到 ~300B，heap churn 大減。
    html += F(
      "<script>"
      "async function bpRefresh(){"
        "try{const r=await fetch('/api/latest');if(!r.ok)return;"
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
          // count 從 >0 變回 0（例如剛清歷史）：reload 切換到 empty state，避免殘留舊卡片
          "location.reload();return;"
        "}}catch(e){}"
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
      "setInterval(bpRefresh,3000);"
      "</script>"
    );

    html += buildPageEnd();
    server->send(200, "text/html", html);
  }

  void handleHistory() {
    String html;
    html.reserve(8192); // CSS ~4.5KB + 20 列表 + danger zone ~3KB
    html = buildPageStart("血壓歷史記錄", "/history");

    html += "<section class='panel history-table'>";
    html += "<div class='section-head'>";
    html += "<h2>所有歷史數據</h2>";
    html += "<a href='/' class='btn btn-ghost'>返回監控</a>";
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
        html += renderTableValueCell(record.systolic, record.valid, &WebHandler::isSystolicAbnormal);
        html += renderTableValueCell(record.diastolic, record.valid, &WebHandler::isDiastolicAbnormal);
        html += renderTableValueCell(record.pulse, record.valid, &WebHandler::isPulseAbnormal);
        html += "<td><a href='/raw_data?id=";
        html += i; // 直接 += int 避免建一次 String(i) 暫物件
        html += "' class='text-link'>查看原始數據</a></td></tr>";
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
    html += "<button type='submit' class='btn btn-danger'>清除記錄</button>";
    html += "</form>";
    html += "</section>";

    html += buildPageEnd();
    server->send(200, "text/html", html);
  }

  void handleHistoryAPI() {
    // ArduinoJson v6：傳 String 進去會 copy 進 doc pool；改用 c_str() 直接引用，
    // 在 single-thread handler 內 BPData 不會被修改，pointer 安全。20 筆省 ~420B pool。
    StaticJsonDocument<3072> doc;
    JsonArray records = doc.createNestedArray("records");

    int recordCount = recordManager->getRecordCount();
    for (int i = 0; i < recordCount; i++) {
      const BPData& record = recordManager->getRecord(i);

      JsonObject recordObj = records.createNestedObject();
      recordObj["timestamp"] = record.timestamp.c_str();
      recordObj["systolic"] = record.systolic;
      recordObj["diastolic"] = record.diastolic;
      recordObj["pulse"] = record.pulse;
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
    String wifiIp = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
    doc["wifi_ip"] = wifiIp.c_str();

    String jsonStr;
    serializeJson(doc, jsonStr);
    server->send(200, "application/json", jsonStr);
  }

  void handleClearHistory() {
    recordManager->clearRecords();
    *lastData = ""; // 同步清掉殘留 raw HTML，避免 dashboard 顯示陳舊資料

    String html = buildPageStart("記錄已清除", "/history", false, "<meta http-equiv='refresh' content='2;url=/history'>");
    html += "<section class='panel danger-zone'>";
    html += "<h2>所有歷史記錄已清除</h2>";
    html += "<p class='helper-text'>正在返回歷史記錄頁面...</p>";
    html += "</section>";
    html += buildPageEnd();

    server->send(200, "text/html", html);
  }

  void handleRawData() {
    String id = server->arg("id");
    if (id.length() > 0) {
      const BPData& record = recordManager->getRecord(id.toInt());
      if (record.valid) {
        String html = buildPageStart("原始數據", "/history");
        html += "<section class='panel raw-data'>";
        html += "<div class='section-head'>";
        html += "<h2>量測原始資料</h2>";
        html += "<a href='/history' class='btn btn-ghost'>返回歷史記錄</a>";
        html += "</div>";
        html += record.rawData;
        html += "</section>";
        html += buildPageEnd();

        server->sendHeader("Content-Type", "text/html; charset=UTF-8");
        server->send(200, "text/html", html);
      } else {
        server->send(404, "text/plain", "找不到該記錄");
      }
    } else {
      server->send(400, "text/plain", "缺少記錄ID");
    }
  }
};

#endif
