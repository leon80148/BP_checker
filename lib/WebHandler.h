#ifndef WEB_HANDLER_H
#define WEB_HANDLER_H

#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "BPRecordManager.h"

// 處理網頁請求的類
class WebHandler {
private:
  WebServer* server;
  Preferences* preferences;
  BP_RecordManager* recordManager;
  String* bp_model;
  String* lastData;
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

  String valueClass(bool abnormal) {
    return abnormal ? "value-bad" : "value-good";
  }

  String statePill(bool abnormal) {
    return abnormal
      ? "<span class='state-pill state-alert'>異常</span>"
      : "<span class='state-pill state-ok'>正常</span>";
  }

  String navLink(const String& href, const String& label, const String& activePath) {
    String cls = "top-nav-link";
    if (href == activePath) {
      cls += " active";
    }
    return "<a class='" + cls + "' href='" + href + "'>" + label + "</a>";
  }

  String sharedStyle() {
    String css = "<style>";
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
    css += ".state-pill{display:inline-block;padding:4px 8px;border-radius:999px;font-size:12px;font-weight:700;}";
    css += ".state-ok{background:rgba(17,138,76,.12);color:var(--success);}";
    css += ".state-alert{background:rgba(217,48,37,.12);color:var(--danger);}";

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
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
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
             String* bp_model, String* lastData, const char** hostname, const char** ap_ssid, const char** ap_password) {
    this->server = server;
    this->preferences = preferences;
    this->recordManager = recordManager;
    this->bp_model = bp_model;
    this->lastData = lastData;
    this->hostname = hostname;
    this->ap_ssid = ap_ssid;
    this->ap_password = ap_password;
  }

  void setupRoutes() {
    server->on("/", HTTP_GET, [this]() { this->handleMonitor(); });
    server->on("/config", HTTP_GET, [this]() { this->handleRoot(); });
    server->on("/configure", HTTP_POST, [this]() { this->handleConfigure(); });
    server->on("/data", HTTP_GET, [this]() {
      server->send(200, "text/plain", *lastData);
    });

    // 添加歷史記錄相關API
    server->on("/history", HTTP_GET, [this]() { this->handleHistory(); });
    server->on("/api/history", HTTP_GET, [this]() { this->handleHistoryAPI(); });
    server->on("/clear_history", HTTP_GET, [this]() { this->handleClearHistory(); });

    // 添加血壓機型號設定路由
    server->on("/bp_model", HTTP_GET, [this]() { this->handleBpModelPage(); });
    server->on("/set_bp_model", HTTP_POST, [this]() { this->handleSetBpModel(); });

    // 添加原始資料顯示路由
    server->on("/raw_data", HTTP_GET, [this]() { this->handleRawData(); });

    // 添加重置路由
    server->on("/reset", HTTP_GET, [this]() {
      preferences->begin("wifi-config", false);
      preferences->clear();
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
    int n = WiFi.scanNetworks();

    String js = "<script>";
    js += "function toggleManualSSID(){";
    js += "var select=document.getElementById('wifi-select');";
    js += "var manualInput=document.getElementById('manual-ssid');";
    js += "if(select.value==='manual'){manualInput.style.display='block';manualInput.required=true;}";
    js += "else{manualInput.style.display='none';manualInput.required=false;}";
    js += "}";
    js += "</script>";

    String html = buildPageStart("WiFi 設定", "/config", false, js);
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

        html += "<option value='" + WiFi.SSID(i) + "'>";
        html += WiFi.SSID(i) + " (" + quality + "%";
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
    html += "<a class='btn btn-secondary scan-refresh' href='/config'>重新掃描 WiFi</a>";
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

    if (new_ssid.length() > 0) {
      preferences->begin("wifi-config", false);
      preferences->putString("ssid", new_ssid);
      preferences->putString("password", new_password);
      preferences->end();

      String html = buildPageStart("設定完成", "/config");
      html += "<section class='panel'>";
      html += "<h2>WiFi 設定已儲存</h2>";
      html += "<p class='helper-text'>設備將重新啟動並嘗試連接新 WiFi。若失敗，可長按 Reset 3 秒還原設定。</p>";
      html += "<ul class='status-list'>";
      html += "<li><span>目標 SSID</span><strong>" + new_ssid + "</strong></li>";
      html += "<li><span>設備網址</span><strong>http://" + String(*hostname) + ".local</strong></li>";
      html += "<li><span>備援 AP</span><strong>" + String(*ap_ssid) + "</strong></li>";
      html += "</ul>";
      html += "</section>";
      html += buildPageEnd();

      server->send(200, "text/html", html);

      delay(2000);
      ESP.restart();
    } else {
      server->send(400, "text/plain", "無效的WiFi設定");
    }
  }

  void handleBpModelPage() {
    String html = buildPageStart("血壓機型號設定", "/bp_model");
    html += "<section class='panel form-shell'>";
    html += "<h2>型號設定</h2>";
    html += "<p class='helper-text'>目前內建 OMRON-HBP9030，若接入其他格式可選擇自定義。</p>";
    html += "<form method='post' action='/set_bp_model'>";
    html += "<label class='field-label' for='model-select'>選擇血壓機型號</label>";
    html += "<select id='model-select' name='model'>";
    html += String("<option value='OMRON-HBP9030'") + (*bp_model == "OMRON-HBP9030" ? " selected" : "") + ">OMRON HBP-9030</option>";
    html += String("<option value='CUSTOM'") + (*bp_model == "CUSTOM" ? " selected" : "") + ">自定義格式</option>";
    html += "</select>";
    html += "<button class='btn' type='submit'>儲存設定</button>";
    html += "</form>";
    html += "<div class='panel' style='margin:14px 0 0;padding:14px 16px;'>";
    html += "<h3>目前型號</h3>";
    html += "<p class='helper-text'>" + *bp_model + "</p>";
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

      String html = buildPageStart("型號設定完成", "/bp_model", false, "<meta http-equiv='refresh' content='2;url=/'>");
      html += "<section class='panel'>";
      html += "<h2>已套用型號：" + new_model + "</h2>";
      html += "<p class='helper-text'>系統將返回監控頁面。</p>";
      html += "</section>";
      html += buildPageEnd();

      server->send(200, "text/html", html);
    } else {
      server->send(400, "text/plain", "無效的型號設定");
    }
  }

  void handleMonitor() {
    String html = buildPageStart("血壓監控儀表板", "/", true);

    int recordCount = recordManager->getRecordCount();
    if (recordCount > 0) {
      BPData latest = recordManager->getLatestRecord();

      bool systolicBad = isSystolicAbnormal(latest.systolic);
      bool diastolicBad = isDiastolicAbnormal(latest.diastolic);
      bool pulseBad = isPulseAbnormal(latest.pulse);

      html += "<section class='panel latest-vitals'>";
      html += "<div class='section-head'><h2>最新量測</h2>";
      html += "<span class='last-updated'>最後更新：" + latest.timestamp + "（每 3 秒刷新）</span></div>";
      html += "<div class='kpi-grid'>";

      html += "<article class='kpi-card'>";
      html += "<div class='kpi-label'><span>收縮壓</span><span>mmHg</span></div>";
      html += "<div class='kpi-value " + valueClass(systolicBad) + "'>" + String(latest.systolic) + "</div>";
      html += statePill(systolicBad);
      html += "</article>";

      html += "<article class='kpi-card'>";
      html += "<div class='kpi-label'><span>舒張壓</span><span>mmHg</span></div>";
      html += "<div class='kpi-value " + valueClass(diastolicBad) + "'>" + String(latest.diastolic) + "</div>";
      html += statePill(diastolicBad);
      html += "</article>";

      html += "<article class='kpi-card'>";
      html += "<div class='kpi-label'><span>脈搏</span><span>bpm</span></div>";
      html += "<div class='kpi-value " + valueClass(pulseBad) + "'>" + String(latest.pulse) + "</div>";
      html += statePill(pulseBad);
      html += "</article>";

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
        BPData record = recordManager->getRecord(i);
        bool sb = isSystolicAbnormal(record.systolic);
        bool db = isDiastolicAbnormal(record.diastolic);
        bool pb = isPulseAbnormal(record.pulse);

        html += "<tr>";
        html += "<td>" + record.timestamp + "</td>";
        html += "<td class='" + valueClass(sb) + "'>" + String(record.systolic) + "</td>";
        html += "<td class='" + valueClass(db) + "'>" + String(record.diastolic) + "</td>";
        html += "<td class='" + valueClass(pb) + "'>" + String(record.pulse) + "</td>";
        html += "</tr>";
      }

      html += "</table></section>";
    } else {
      html += "<section class='panel latest-vitals'>";
      html += "<h2>最新量測</h2>";
      html += "<p class='helper-text'>尚未收到血壓數據。請確認血壓機連線與 TTL 腳位後再量測。</p>";
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
    html += "<li><span>血壓機型號</span><strong>" + *bp_model + "</strong></li>";
    html += "<li><span>WiFi IP</span><strong>" + wifiIp + "</strong></li>";
    html += "<li><span>可訪問網址</span><strong>http://" + String(*hostname) + ".local</strong></li>";
    html += "<li><span>AP 熱點</span><strong>" + String(*ap_ssid) + " (" + String(*ap_password) + ")</strong></li>";
    html += "</ul>";
    html += "</section>";

    html += "<section class='panel danger-zone'>";
    html += "<h3>維護操作</h3>";
    html += "<p class='helper-text'>若要重新配網，可重置 WiFi 設定並重啟。</p>";
    html += "<a href='/reset' class='btn btn-danger'>重置 WiFi 設定</a>";
    html += "</section>";

    html += buildPageEnd();
    server->send(200, "text/html", html);
  }

  void handleHistory() {
    String html = buildPageStart("血壓歷史記錄", "/history");

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
        BPData record = recordManager->getRecord(i);
        bool sb = isSystolicAbnormal(record.systolic);
        bool db = isDiastolicAbnormal(record.diastolic);
        bool pb = isPulseAbnormal(record.pulse);

        html += "<tr>";
        html += "<td>" + record.timestamp + "</td>";
        html += "<td class='" + valueClass(sb) + "'>" + String(record.systolic) + "</td>";
        html += "<td class='" + valueClass(db) + "'>" + String(record.diastolic) + "</td>";
        html += "<td class='" + valueClass(pb) + "'>" + String(record.pulse) + "</td>";
        html += "<td><a href='/raw_data?id=" + String(i) + "' class='text-link'>查看原始數據</a></td>";
        html += "</tr>";
      }
    } else {
      html += "<tr><td colspan='5'>尚無歷史記錄</td></tr>";
    }

    html += "</table>";
    html += "</section>";

    html += "<section class='panel danger-zone'>";
    html += "<h3>危險操作</h3>";
    html += "<p class='helper-text'>此操作會清除全部歷史資料且無法復原。</p>";
    html += "<a href='/clear_history' class='btn btn-danger' onclick=\"return confirm('確定要清除所有歷史記錄嗎？');\">清除記錄</a>";
    html += "</section>";

    html += buildPageEnd();
    server->send(200, "text/html", html);
  }

  void handleHistoryAPI() {
    StaticJsonDocument<2048> doc;
    JsonArray records = doc.createNestedArray("records");

    int recordCount = recordManager->getRecordCount();
    for (int i = 0; i < recordCount; i++) {
      BPData record = recordManager->getRecord(i);

      JsonObject recordObj = records.createNestedObject();
      recordObj["timestamp"] = record.timestamp;
      recordObj["systolic"] = record.systolic;
      recordObj["diastolic"] = record.diastolic;
      recordObj["pulse"] = record.pulse;
    }

    String jsonStr;
    serializeJson(doc, jsonStr);
    server->send(200, "application/json", jsonStr);
  }

  void handleClearHistory() {
    recordManager->clearRecords();

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
      BPData record = recordManager->getRecord(id.toInt());
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
