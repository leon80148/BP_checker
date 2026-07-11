#ifndef WEB_HANDLER_H
#define WEB_HANDLER_H

#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "BoundedWebServer.h"
#include "BPRecordManager.h"
#include "BP_Parser.h"
#include "CsvExport.h"
#include "DeviceSecurity.h"
#include "BuildInfo.h"
#include "MeasurementPolicy.h"
#include "WebAccessPolicy.h"
#include "transports/MonitorTransport.h"

// 處理網頁請求的類
class WebHandler {
private:
  bp_web::BoundedWebServer* server;
  DeviceSecurity* deviceSecurity;
  Preferences* preferences;
  BP_RecordManager* recordManager;
  BP_Parser* bpParser;
  String* bp_model;
  String* lastData;
  String* transportName;
  String* transportStatus;
  MonitorTransport* monitorTransport;
  MonotonicMillis64* uptimeClock;
  MeasurementPolicyStore* measurementPolicyStore;
  // 全域 ap_*/hostname 是 const char* 編譯期常數（bp_checker.ino），
  // 用單層 const char* 即可，省一層 indirection
  const char* hostname;
  const char* ap_ssid;

  static void restartDevice(void*) {
    ESP.restart();
  }

  static void secureWipeString(String& value) {
    volatile char* bytes = value.begin();
    size_t length = value.length();
    while (length-- != 0) *bytes++ = 0;
    value = String();
  }

  static bool securityOperationSucceeded(DeviceSecurityResult result) {
    return result == DeviceSecurityResult::OK;
  }

  static void appendUInt64(String& out, uint64_t value) {
    char encoded[24];
    if (formatOpaqueSequence(value, encoded, sizeof(encoded))) {
      out += encoded;
    }
  }

  static void setUInt64Json(JsonVariant target, uint64_t value) {
    char encoded[24];
    if (formatOpaqueSequence(value, encoded, sizeof(encoded))) {
      // A mutable char buffer is duplicated into the JsonDocument. Keeping
      // the value as decimal text preserves the full uint64 range in JS.
      target.set(encoded);
    } else {
      target.set(nullptr);
    }
  }

  const char* reviewClass(MeasurementReviewState state) const {
    if (state == MeasurementReviewState::INVALID) return "value-na";
    return state == MeasurementReviewState::WITHIN_REFERENCE
      ? "value-good" : "value-bad";
  }

  const MeasurementPolicyConfig& activePolicy() const {
    return measurementPolicyStore->config();
  }

  bool isTransportConnected() const {
    if (monitorTransport == nullptr) return false;
    const MonitorTransportState state = monitorTransport->state();
    return state == TRANSPORT_STATE_READY ||
           state == TRANSPORT_STATE_RECEIVING;
  }

  MeasurementFreshnessState latestFreshness(uint64_t nowMs) const {
    MeasurementFreshnessInput input;
    input.hasRecord = recordManager->getRecordCount() > 0;
    if (input.hasRecord) {
      input.valid = recordManager->getLatestRecord().valid;
    }
    input.receivedThisBoot = recordManager->latestReceivedThisBoot();
    input.transportConnected = isTransportConnected();
    input.nowMs = nowMs;
    input.staleAfterMs = activePolicy().staleAfterMs;
    uint64_t receiveAge = 0;
    if (recordManager->lastSuccessfulReceiveAgeMs(nowMs, receiveAge)) {
      input.lastSuccessfulReceiveMs = nowMs - receiveAge;
    }
    return measurementFreshness(input);
  }

  const char* sanitizedDiagnosticState() const {
    static constexpr const char* kStates[] = {
      "valid", "storage_error", "invalid_timestamp", "device_error",
      "out_of_range", "unsupported_format", "unsupported_model",
      "overflow", "discontinuity", "malformed"
    };
    for (const char* state : kStates) {
      char marker[48];
      const int length = snprintf(marker, sizeof(marker), "data-status='%s'",
                                  state);
      if (length > 0 && static_cast<size_t>(length) < sizeof(marker) &&
          lastData->indexOf(marker) >= 0) {
        return state;
      }
    }
    return lastData->isEmpty() ? "waiting" : "unavailable";
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
  void renderTableValueCell(String& out, int value, bool valid) const {
    bool ok = valid && value > 0;
    if (!ok) { out += "<td class='value-na'>—</td>"; return; }
    out += "<td>";
    out += value; // int append 用 String 內建 overload，免 String(value) 暫物件
    out += "</td>";
  }

  // KPI 卡片：valueOk=false 顯示 "—" + 中性提示，避免無效值看似可用。
  // 直接 append 進 out，省每次呼叫一個 ~320B 的中介 String（dashboard 一次 render 3 次）
  void renderKpiCard(String& out, const char* idVal, const char* idPill,
                     const char* label, const char* unit,
                     int value, bool valueOk,
                     MeasurementReviewState state) const {
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
      out += reviewClass(state);
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
      out += state == MeasurementReviewState::WITHIN_REFERENCE
        ? "state-ok" : "state-alert";
      out += "'>";
      out += measurementReviewLabel(state);
      out += "</span>";
    }
    out += "</article>";
  }

  // 直接 append 進 out，省每次呼叫一個 ~80B 的中介 String（buildPageStart 4 次 / 渲染）
  void navLink(String& out, const String& href, const String& label, const String& activePath) const {
    out += "<a class='top-nav-link";
    if (href == activePath) out += " active";
    out += "'";
    if (href == activePath) out += " aria-current='page'";
    out += " href='";
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
    css += "a:focus-visible,button:focus-visible,input:focus-visible,select:focus-visible,summary:focus-visible{outline:3px solid #111827;outline-offset:3px;}";

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
    css += ".freshness-banner{padding:12px 14px;border:2px solid #35578c;border-radius:12px;background:#eef5ff;font-weight:800;margin-bottom:14px;}";
    css += ".freshness-banner[data-state='stale'],.freshness-banner[data-state='disconnected'],.freshness-banner[data-state='invalid']{border-color:var(--danger);background:#fff4f3;}";

    css += ".recent-table table,.history-table table{width:100%;border-collapse:collapse;font-size:14px;overflow:hidden;border-radius:12px;}";
    css += ".recent-table th,.recent-table td,.history-table th,.history-table td{padding:11px 10px;border-bottom:1px solid #e5edf7;text-align:center;}";
    css += ".recent-table th,.history-table th{background:#eff5ff;font-size:12px;letter-spacing:.06em;text-transform:uppercase;color:#4d6282;}";
    css += ".recent-table tr:nth-child(even),.history-table tr:nth-child(even){background:#fbfdff;}";
    css += ".table-scroll{max-width:100%;overflow-x:auto;-webkit-overflow-scrolling:touch;}";
    css += "caption{text-align:left;font-weight:800;padding:0 0 10px;color:#223a5f;}";

    css += ".status-list{list-style:none;margin:0;padding:0;display:grid;gap:8px;}";
    css += ".status-list li{display:flex;justify-content:space-between;gap:12px;padding:8px 10px;border-radius:10px;background:#f7faff;border:1px solid #e7eef9;}";

    css += ".diagnostic-data summary{cursor:pointer;font-weight:700;outline:none;}";
    css += ".diagnostic-data[open] summary{margin-bottom:12px;}";

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
    html = "<!DOCTYPE html><html lang='zh-Hant'><head><meta charset='UTF-8'>";
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
    html += "<nav class='top-nav' aria-label='主要導覽'>";
    if (bp_web::surfaceVisible(server->currentRole(),
                               bp_web::WebSurface::MONITOR_NAV)) {
      navLink(html, "/", "監控", activePath);
    }
    if (bp_web::surfaceVisible(server->currentRole(),
                               bp_web::WebSurface::HISTORY_NAV)) {
      navLink(html, "/history", "歷史記錄", activePath);
    }
    if (bp_web::surfaceVisible(server->currentRole(),
                               bp_web::WebSurface::ADMIN_WIFI_NAV)) {
      navLink(html, "/config", "WiFi 設定", activePath);
    }
    if (bp_web::surfaceVisible(server->currentRole(),
                               bp_web::WebSurface::ADMIN_MODEL_NAV)) {
      navLink(html, "/bp_model", "型號設定", activePath);
    }
    if (bp_web::surfaceVisible(server->currentRole(),
                               bp_web::WebSurface::ADMIN_SECURITY_NAV)) {
      navLink(html, "/security", "管理者安全設定", activePath);
    }
    if (bp_web::surfaceVisible(server->currentRole(),
                               bp_web::WebSurface::ADMIN_POLICY_NAV)) {
      navLink(html, "/measurement_policy", "量測政策", activePath);
    }
    html += "</nav>";
    html += "<main id='main-content'>";
    return html;
  }

  // 回 const char*：caller 用 `html += buildPageEnd()` 是 String += const char*，
  // 不會建臨時 String（每頁 render 省一個 ~30 byte alloc）
  const char* buildPageEnd() const {
    return "</main></div></body></html>";
  }

public:
  WebHandler(bp_web::BoundedWebServer* server,
             DeviceSecurity* deviceSecurity,
             Preferences* preferences, BP_RecordManager* recordManager,
             BP_Parser* bpParser,
             String* bp_model, String* lastData, String* transportName, String* transportStatus,
             MonitorTransport* monitorTransport,
             MonotonicMillis64* uptimeClock,
             MeasurementPolicyStore* measurementPolicyStore,
             const char* hostname, const char* ap_ssid)
    : server(server),
      deviceSecurity(deviceSecurity),
      preferences(preferences),
      recordManager(recordManager),
      bpParser(bpParser),
      bp_model(bp_model),
      lastData(lastData),
      transportName(transportName),
      transportStatus(transportStatus),
      monitorTransport(monitorTransport),
      uptimeClock(uptimeClock),
      measurementPolicyStore(measurementPolicyStore),
      hostname(hostname),
      ap_ssid(ap_ssid) {}

  void setupRoutes() {
    server->on("/claim", HTTP_GET, [this]() { this->handleClaimPage(); });
    server->on("/claim", HTTP_POST, [this]() { this->handleClaim(); });
    server->on("/", HTTP_GET, [this]() { this->handleMonitor(); });
    server->on("/config", HTTP_GET, [this]() { this->handleRoot(); });
    server->on("/configure", HTTP_POST, [this]() { this->handleConfigure(); });
    // /data 只回傳去識別化的結構化接收診斷 HTML 片段。
    server->on("/data", HTTP_GET, [this]() {
      server->send(200, "text/html; charset=UTF-8", *lastData);
    });

    // 添加歷史記錄相關API
    server->on("/history", HTTP_GET, [this]() { this->handleHistory(); });
    // CSV 匯出與 /api/history 同為 staff/admin authenticated read。
    server->on("/export.csv", HTTP_GET, [this]() { this->handleExportCsv(); });
    server->on("/api/history", HTTP_GET, [this]() { this->handleHistoryAPI(); });
    server->on("/api/latest", HTTP_GET, [this]() { this->handleLatestAPI(); });
    // 破壞性操作改為 POST，避免瀏覽器 link prefetch、爬蟲、誤點 GET 觸發。
    server->on("/clear_history", HTTP_POST, [this]() { this->handleClearHistory(); });

    // 添加血壓機型號設定路由
    server->on("/bp_model", HTTP_GET, [this]() { this->handleBpModelPage(); });
    server->on("/set_bp_model", HTTP_POST, [this]() { this->handleSetBpModel(); });

    server->on("/security", HTTP_GET,
               [this]() { this->handleSecurityPage(); });
    server->on("/rotate_credentials", HTTP_POST,
               [this]() { this->handleRotateCredentials(); });
    server->on("/measurement_policy", HTTP_GET,
               [this]() { this->handleMeasurementPolicyPage(); });
    server->on("/set_measurement_policy", HTTP_POST,
               [this]() { this->handleSetMeasurementPolicy(); });
    server->on("/reset", HTTP_POST, [this]() { this->handleReset(); });
  }

private:
  // 以下 handle* 都只在 setupRoutes 內以 lambda 連接到 route，無外部 caller。
  void handleClaimPage() {
    const bool recovery = deviceSecurity != nullptr &&
      deviceSecurity->claimState() == DeviceClaimState::CLAIMED &&
      server->currentRequestInterface() ==
        bp_web::RequestInterface::RECOVERY_AP;
    String html;
    html.reserve(2048);
    html =
      "<!DOCTYPE html><html lang='zh-Hant'><head><meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>BP Checker 安全設定</title></head><body><main><h1>";
    html += recovery ? "復原管理者存取" : "啟用 BP Checker";
    html += recovery
      ? "</h1><p>請輸入事先安全保存的實體復原碼；成功後管理者與工作人員密碼都會輪替。</p>"
      : "</h1><p>請輸入隨裝置交付的一次性啟用碼。此步驟只能在裝置設定熱點上完成。</p>";
    html +=
      "<form method='post' action='/claim'>"
      "<label for='bootstrap-token'>一次性啟用碼</label>"
      "<input id='bootstrap-token' type='password' name='token' "
      "autocomplete='one-time-code' required maxlength='22'>"
      "<button type='submit'>啟用裝置</button></form>"
      "<p>目前熱點：<strong>";
    html += htmlEscape(String(ap_ssid));
    html += "</strong></p></main></body></html>";
    server->send(200, "text/html; charset=UTF-8", html);
    secureWipeString(html);
  }

  void handleClaim() {
    String token = server->arg("token");
    const bool onProvisioningAp =
      server->currentRequestInterface() ==
        bp_web::RequestInterface::PROVISIONING_AP;
    const bool onRecoveryAp =
      server->currentRequestInterface() ==
        bp_web::RequestInterface::RECOVERY_AP;
    const bool recovering = deviceSecurity != nullptr &&
      deviceSecurity->claimState() == DeviceClaimState::CLAIMED;
    const DeviceSecurityResult result = deviceSecurity == nullptr
      ? DeviceSecurityResult::INVALID_STATE
      : recovering
        ? deviceSecurity->recoverWithBootstrap(token, onRecoveryAp)
        : deviceSecurity->claimBootstrap(token, onProvisioningAp, false);
    const bool accepted = securityOperationSucceeded(result);
    (void)server->recordClaimResult(accepted, millis());
    secureWipeString(token);
    if (!accepted) {
      server->send(result == DeviceSecurityResult::DENIED ? 403 : 503,
                   "text/plain; charset=UTF-8",
                   result == DeviceSecurityResult::DENIED
                     ? "啟用碼錯誤或啟用環境不符"
                     : "裝置安全狀態未能完成寫入");
      return;
    }

    String html;
    html.reserve(3072);
    html =
      "<!DOCTYPE html><html lang='zh-Hant'><head><meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>裝置存取已更新</title></head><body><main><h1>";
    html += recovering ? "管理者存取已復原" : "裝置已啟用";
    html += "</h1>"
      "<p>請立即將下列存取資料存入診所密碼管理器。</p>"
      "<dl><dt>管理者帳號</dt><dd>admin</dd><dt>管理者密碼</dt><dd><code>";
    html += deviceSecurity->secret(DeviceSecretKind::ADMIN);
    html += "</code></dd><dt>工作人員帳號</dt><dd>staff</dd>"
            "<dt>工作人員密碼</dt><dd><code>";
    html += deviceSecurity->secret(DeviceSecretKind::STAFF);
    html += "</code></dd><dt>復原 AP 密碼</dt><dd><code>";
    html += deviceSecurity->secret(DeviceSecretKind::AP);
    html += "</code></dd></dl>"
            "<p><strong>部署前請先建立實體復原碼：</strong>以管理者登入安全設定，輪替並保存「實體復原碼」。每次使用復原碼後都必須再建立一組。</p>"
            "<p><a href='/config'>使用管理者帳號繼續設定 WiFi</a> · "
            "<a href='/security'>開啟安全設定</a></p>"
            "</main></body></html>";
    server->send(200, "text/html; charset=UTF-8", html);
    secureWipeString(html);
  }

  void handleSecurityPage() {
    if (deviceSecurity == nullptr ||
        deviceSecurity->availability() != DeviceSecurityAvailability::READY ||
        deviceSecurity->claimState() != DeviceClaimState::CLAIMED) {
      server->send(503, "text/plain; charset=UTF-8",
                   "裝置安全狀態目前不可用");
      return;
    }
    String html = buildPageStart("裝置安全", "/security");
    html += "<section class='panel form-shell'><h2>存取憑證</h2>";
    html += "<p class='helper-text'>請保存在診所核准的密碼管理器；Basic 憑證只允許用於隔離的 WPA2 診所網路。</p>";
    html += "<ul class='status-list'><li><span>管理者帳號</span><strong>admin</strong></li>";
    html += "<li><span>管理者密碼</span><strong><code>";
    html += deviceSecurity->secret(DeviceSecretKind::ADMIN);
    html += "</code></strong></li><li><span>工作人員帳號</span><strong>staff</strong></li>";
    html += "<li><span>工作人員密碼</span><strong><code>";
    html += deviceSecurity->secret(DeviceSecretKind::STAFF);
    html += "</code></strong></li><li><span>復原 AP 密碼</span><strong><code>";
    html += deviceSecurity->secret(DeviceSecretKind::AP);
    html += "</code></strong></li><li><span>實體復原碼</span><strong>";
    if (deviceSecurity->tokenConsumed()) {
      html += "尚未建立可用復原碼";
    } else {
      html += "<code>";
      html += deviceSecurity->secret(DeviceSecretKind::BOOTSTRAP);
      html += "</code>";
    }
    html += "</strong></li></ul>";
    if (deviceSecurity->tokenConsumed()) {
      html += "<p class='helper-text'><strong>部署前請先建立實體復原碼。</strong>下方選擇「實體復原碼」並立即輪替。</p>";
    } else {
      html += "<p class='helper-text'>復原碼已啟用；請離線保管。成功復原後必須立即輪替新碼。</p>";
    }
    html += "</section>";
    html += "<section class='panel form-shell'><h2>輪替憑證</h2>";
    html += "<form method='post' action='/rotate_credentials'>";
    html += "<label class='field-label' for='credential-kind'>憑證類型</label>";
    html += "<select id='credential-kind' name='kind'>"
            "<option value='admin'>管理者</option>"
            "<option value='staff'>工作人員</option>"
            "<option value='bootstrap'>實體復原碼</option>"
            "<option value='ap'>設定熱點密碼</option></select>";
    html += "<button class='btn' type='submit'>立即輪替</button></form></section>";
    html += buildPageEnd();
    server->send(200, "text/html; charset=UTF-8", html);
    secureWipeString(html);
  }

  void handleRotateCredentials() {
    const String kind = server->arg("kind");
    DeviceSecretKind secretKind = DeviceSecretKind::ADMIN;
    bool validKind = true;
    if (kind == "admin") secretKind = DeviceSecretKind::ADMIN;
    else if (kind == "staff") secretKind = DeviceSecretKind::STAFF;
    else if (kind == "bootstrap") secretKind = DeviceSecretKind::BOOTSTRAP;
    else if (kind == "ap") secretKind = DeviceSecretKind::AP;
    else validKind = false;
    if (!validKind || deviceSecurity == nullptr) {
      server->send(400, "text/plain; charset=UTF-8", "無效的憑證類型");
      return;
    }

    const DeviceSecurityResult result = deviceSecurity->rotateSecret(secretKind);
    if (!securityOperationSucceeded(result)) {
      if (deviceSecurity->availability() ==
          DeviceSecurityAvailability::REBOOT_REQUIRED) {
        (void)server->deferAfterResponse(&WebHandler::restartDevice, nullptr);
      }
      server->send(503, "text/plain; charset=UTF-8",
                   "憑證輪替未能確認完成；裝置可能重新啟動以復原");
      return;
    }

    const bool restartRequired = bp_web::credentialRotationRequiresRestart(
      secretKind, server->currentRequestInterface());
    const bool restartScheduled = !restartRequired ||
      server->deferAfterResponse(&WebHandler::restartDevice, nullptr);

    String html = buildPageStart("憑證已輪替", "/security");
    html += "<section class='panel'><h2>新的憑證</h2><p><code>";
    html += deviceSecurity->secret(secretKind);
    html += "</code></p><p class='helper-text'>";
    if (restartRequired && restartScheduled) {
      html += "AP 密碼已輪替；回應完成後裝置將重新啟動。請安全保存新值，並依實體復原流程重新開啟熱點。";
    } else if (restartRequired) {
      html += "AP 密碼已輪替，但無法排程重啟；請安全保存新值並手動重新啟動裝置，舊 AP 密碼才會停止生效。";
    } else {
      html += "舊憑證已立即失效；請安全保存新值。";
    }
    html += "</p>";
    html += "<a class='btn' href='/security'>返回安全設定</a></section>";
    html += buildPageEnd();
    server->send(restartScheduled ? 200 : 503,
                 "text/html; charset=UTF-8", html);
    secureWipeString(html);
  }

  void handleMeasurementPolicyPage() {
    const MeasurementPolicyConfig& policy = activePolicy();
    String html = buildPageStart("量測複核政策", "/measurement_policy");
    html.reserve(12288);
    html += "<section class='panel form-shell'><h2>目前政策</h2>";
    html += "<p class='helper-text'>此設定由診所管理者維護；每次變更必須使用更大的政策版本。";
    html += measurementReferencePolicyName();
    html += "。</p><ul class='status-list'><li><span>政策名稱</span><strong>";
    html += htmlEscape(String(policy.policyName));
    html += "</strong></li><li><span>政策版本</span><strong>";
    html += policy.policyVersion;
    html += "</strong></li></ul></section>";

    const bool canUpdatePolicy = bp_web::surfaceVisible(
      server->currentRole(), bp_web::WebSurface::POLICY_UPDATE_CONTROL);
    if (canUpdatePolicy && policy.policyVersion != UINT32_MAX) {
      html += "<section class='panel form-shell'><h2>更新政策</h2>";
      html += "<form method='post' action='/set_measurement_policy'>";
      html += "<label class='field-label' for='policy-name'>政策名稱（英數、空格、._-()）</label>";
      html += "<input id='policy-name' name='policy_name' maxlength='32' required value='";
      html += htmlEscape(String(policy.policyName));
      html += "'>";
      html += "<label class='field-label' for='policy-version'>新政策版本</label>";
      html += "<input id='policy-version' name='policy_version' inputmode='numeric' required value='";
      html += policy.policyVersion + 1U;
      html += "'>";
      html += "<label class='field-label' for='review-systolic'>收縮壓複核門檻</label>";
      html += "<input id='review-systolic' name='review_systolic' inputmode='numeric' required value='";
      html += policy.reviewSystolic;
      html += "'>";
      html += "<label class='field-label' for='review-diastolic'>舒張壓複核門檻</label>";
      html += "<input id='review-diastolic' name='review_diastolic' inputmode='numeric' required value='";
      html += policy.reviewDiastolic;
      html += "'>";
      html += "<label class='field-label' for='pulse-low'>脈搏下限</label>";
      html += "<input id='pulse-low' name='pulse_low' inputmode='numeric' required value='";
      html += policy.reviewPulseLow;
      html += "'>";
      html += "<label class='field-label' for='pulse-high'>脈搏上限</label>";
      html += "<input id='pulse-high' name='pulse_high' inputmode='numeric' required value='";
      html += policy.reviewPulseHigh;
      html += "'>";
      html += "<label class='field-label' for='urgent-systolic'>收縮壓緊急提示邊界（高於此值）</label>";
      html += "<input id='urgent-systolic' name='urgent_systolic' inputmode='numeric' required value='";
      html += policy.urgentSystolic;
      html += "'>";
      html += "<label class='field-label' for='urgent-diastolic'>舒張壓緊急提示邊界（高於此值）</label>";
      html += "<input id='urgent-diastolic' name='urgent_diastolic' inputmode='numeric' required value='";
      html += policy.urgentDiastolic;
      html += "'>";
      html += "<label class='field-label' for='stale-seconds'>量測逾時秒數</label>";
      html += "<input id='stale-seconds' name='stale_seconds' inputmode='numeric' required value='";
      html += policy.staleAfterMs / 1000U;
      html += "'><button class='btn' type='submit'>驗證並儲存政策</button>";
      html += "</form></section>";
    } else if (canUpdatePolicy && policy.policyVersion == UINT32_MAX) {
      html += "<section class='panel form-shell' role='status'>";
      html += "<h2>政策版本已達上限</h2>";
      html += "<p class='helper-text'>此政策無法再原地更新；請依受控維護流程建立新的政策儲存世代。</p>";
      html += "</section>";
    }
    html += buildPageEnd();
    server->send(200, "text/html; charset=UTF-8", html);
  }

  void handleSetMeasurementPolicy() {
    if (!bp_web::surfaceVisible(server->currentRole(),
                                bp_web::WebSurface::POLICY_UPDATE_CONTROL)) {
      server->send(403, "text/plain; charset=UTF-8", "僅限管理者更新政策");
      return;
    }
    static constexpr const char* kFields[] = {
      "policy_version", "review_systolic", "review_diastolic",
      "pulse_low", "pulse_high", "urgent_systolic",
      "urgent_diastolic", "stale_seconds"
    };
    uint32_t values[sizeof(kFields) / sizeof(kFields[0])] = {};
    bool parsed = true;
    for (size_t i = 0; i < sizeof(kFields) / sizeof(kFields[0]); ++i) {
      const String field = server->arg(kFields[i]);
      if (!parseMeasurementPolicyUnsigned(field.c_str(), values[i])) {
        parsed = false;
      }
    }
    MeasurementPolicyConfig candidate = activePolicy();
    const String policyName = server->arg("policy_name");
    parsed = parsed && copyMeasurementPolicyName(candidate,
                                                  policyName.c_str());
    for (size_t i = 1; i <= 6; ++i) {
      if (values[i] > static_cast<uint32_t>(INT_MAX)) parsed = false;
    }
    if (values[7] > 86400U) parsed = false;
    if (!parsed) {
      server->send(400, "text/plain; charset=UTF-8", "量測政策欄位格式無效");
      return;
    }
    candidate.policyVersion = values[0];
    candidate.reviewSystolic = static_cast<int>(values[1]);
    candidate.reviewDiastolic = static_cast<int>(values[2]);
    candidate.reviewPulseLow = static_cast<int>(values[3]);
    candidate.reviewPulseHigh = static_cast<int>(values[4]);
    candidate.urgentSystolic = static_cast<int>(values[5]);
    candidate.urgentDiastolic = static_cast<int>(values[6]);
    candidate.staleAfterMs = values[7] * 1000U;
    const MeasurementPolicyResult result = measurementPolicyStore->update(candidate);
    if (result == MeasurementPolicyResult::INVALID_POLICY) {
      server->send(400, "text/plain; charset=UTF-8",
                   "量測政策門檻或版本無效；未變更目前政策");
      return;
    }
    if (result != MeasurementPolicyResult::OK) {
      server->send(503, "text/plain; charset=UTF-8",
                   "量測政策未能確認持久化；裝置維持安全狀態");
      return;
    }
    String html = buildPageStart("量測政策已更新", "/measurement_policy",
                                 false,
                                 "<meta http-equiv='refresh' content='2;url=/measurement_policy'>");
    html += "<section class='panel'><h2>政策已驗證並持久化</h2>";
    html += "<p class='helper-text'>所有後續 UI 與 API 複核、新鮮度判定立即使用新版本。</p>";
    html += "</section>";
    html += buildPageEnd();
    server->send(200, "text/html; charset=UTF-8", html);
  }

  void handleReset() {
    if (deviceSecurity == nullptr) {
      server->send(503, "text/plain; charset=UTF-8",
                   "裝置安全狀態目前不可用");
      return;
    }
    const DeviceSecurityResult result =
      deviceSecurity->requestWipe(DeviceWipeKind::NETWORK);
    if (!securityOperationSucceeded(result)) {
      if (deviceSecurity->availability() ==
          DeviceSecurityAvailability::REBOOT_REQUIRED) {
        (void)server->deferAfterResponse(&WebHandler::restartDevice, nullptr);
      }
      server->send(503, "text/plain; charset=UTF-8",
                   "無法確認網路清除狀態；請依維護流程重新啟動");
      return;
    }
    if (!server->deferAfterResponse(&WebHandler::restartDevice, nullptr)) {
      server->send(503, "text/plain; charset=UTF-8",
                   "清除已排程；請手動重新啟動裝置");
      return;
    }

    String html = buildPageStart("網路重置已排程", "/config");
    html += "<section class='panel danger-zone'><h2>網路憑證將在重啟前清除</h2>";
    html += "<p class='helper-text'>重啟後不會自動開放熱點；請在裝置旁以實體按鍵開啟限時復原模式。</p>";
    html += "</section>";
    html += buildPageEnd();
    server->send(200, "text/html; charset=UTF-8", html);
    secureWipeString(html);
  }

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
        "var manualLabel=document.getElementById('manual-ssid-label');"
        "if(select.value==='manual'){manualInput.style.display='block';manualLabel.style.display='block';manualInput.required=true;}"
        "else{manualInput.style.display='none';manualLabel.style.display='none';manualInput.required=false;}"
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

    html += "<label id='manual-ssid-label' class='field-label' for='manual-ssid' style='display:none'>手動輸入 WiFi 名稱</label>";
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
      html += "<li><span>IP 位址</span><strong>";
      html += WiFi.localIP().toString();
      html += "</strong></li>";
      html += "<li><span>瀏覽器存取</span><strong>http://";
      html += hostname;
      html += ".local</strong></li>";
      html += "</ul></div>";
    }

    html += "</section>";

    html += "<section class='panel form-shell'>";
    html += "<h2>存取安全</h2>";
    html += "<p class='helper-text'>管理與工作人員帳號由每台裝置的獨立憑證保護；請勿共用或貼在公共區域。</p>";
    html += "<a class='btn' href='/security'>管理裝置憑證</a>";
    html += "</section>";

    html += buildPageEnd();

    server->send(200, "text/html; charset=UTF-8", html);
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
      server->send(400, "text/plain; charset=UTF-8", "無效的WiFi設定（SSID 長度需 1-32）");
      return;
    }
    if (new_password.length() > 63) {
      server->send(400, "text/plain; charset=UTF-8", "無效的WiFi設定（密碼長度需 0-63）");
      return;
    }

    bool stored = preferences->begin("wifi-config", false);
    if (stored) {
      (void)preferences->putString("ssid", new_ssid.c_str());
      (void)preferences->putString("password", new_password.c_str());
      preferences->end();
      stored = preferences->begin("wifi-config", true);
    }
    String observedSsid;
    String observedPassword;
    if (stored) {
      observedSsid = preferences->getString("ssid", "");
      observedPassword = preferences->getString("password", "");
      stored = preferences->isKey("ssid") &&
               preferences->isKey("password") &&
               observedSsid == new_ssid &&
               observedPassword == new_password;
      preferences->end();
    }
    secureWipeString(observedPassword);
    secureWipeString(new_password);
    if (!stored) {
      secureWipeString(new_ssid);
      server->send(503, "text/plain; charset=UTF-8",
                   "WiFi 設定未能安全寫入，裝置不會重新啟動");
      return;
    }

    String html = buildPageStart("設定完成", "/config");
    html += "<section class='panel'>";
    html += "<h2>WiFi 設定已儲存</h2>";
    html += "<p class='helper-text'>設備將重新啟動並嘗試連接新 WiFi。若連線失敗，請在裝置旁長按 Reset 3 秒，開啟 10 分鐘復原熱點。</p>";
    html += "<ul class='status-list'>";
    html += "<li><span>目標 SSID</span><strong>";
    html += htmlEscape(new_ssid);
    html += "</strong></li>";
    html += "<li><span>設備網址</span><strong>http://";
    html += hostname;
    html += ".local</strong></li>";
    html += "<li><span>復原 AP（需實體啟動）</span><strong>";
    html += ap_ssid;
    html += "</strong></li>";
    html += "</ul>";
    html += "</section>";
    html += buildPageEnd();

    if (!server->deferAfterResponse(&WebHandler::restartDevice, nullptr)) {
      secureWipeString(new_ssid);
      secureWipeString(html);
      server->send(503, "text/plain; charset=UTF-8",
                   "設定已儲存；請手動重新啟動裝置");
      return;
    }
    server->send(200, "text/html; charset=UTF-8", html);
    secureWipeString(new_ssid);
    secureWipeString(html);
  }

  void handleBpModelPage() {
    String html = buildPageStart("血壓機型號設定", "/bp_model");
    html += "<section class='panel form-shell'>";
    html += "<h2>型號設定</h2>";
    html += "<p class='helper-text'>正式版只接受經驗證的 OMRON HBP-9030 USB 輸出格式 5。</p>";
    html += "<form method='post' action='/set_bp_model'>";
    html += "<label class='field-label' for='model-select'>選擇血壓機型號</label>";
    html += "<select id='model-select' name='model'>";
    html += "<option value='OMRON-HBP9030'";
    if (*bp_model == "OMRON-HBP9030") html += " selected";
    html += ">OMRON HBP-9030</option>";
    html += "</select>";
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
    String new_model = server->arg("model");

    if (bp_web::isProductionModelAllowed(new_model.c_str())) {
      bool stored = preferences->begin("wifi-config", false);
      if (stored) {
        (void)preferences->putString("bp_model", new_model.c_str());
        preferences->end();
        stored = preferences->begin("wifi-config", true);
      }
      String observedModel;
      if (stored) {
        observedModel = preferences->getString("bp_model", "");
        stored = preferences->isKey("bp_model") &&
                 observedModel == new_model;
        preferences->end();
      }
      if (!stored) {
        server->send(503, "text/plain; charset=UTF-8",
                     "型號設定未能安全寫入；目前解析設定不變");
        return;
      }
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
    String html = buildPageStart("血壓監控儀表板", "/", false);
    html.reserve(14336);

    const uint64_t nowMs = uptimeClock == nullptr ? 0 : uptimeClock->nowMs();
    const int recordCount = recordManager->getRecordCount();
    const MeasurementFreshnessState freshness = latestFreshness(nowMs);
    html += "<div id='measurement-freshness' class='freshness-banner' data-state='";
    html += measurementFreshnessCode(freshness);
    html += "' role='status' aria-live='polite'>資料新鮮度：";
    html += measurementFreshnessLabel(freshness);
    html += "</div>";

    if (recordCount > 0) {
      const BPData& latest = recordManager->getLatestRecord();
      const bool sysOk = latest.valid && latest.systolic > 0;
      const bool diaOk = latest.valid && latest.diastolic > 0;
      const bool pulOk = latest.valid && latest.pulse > 0;
      const MeasurementReviewState review =
        classifyMeasurement(latest, activePolicy());

      html += "<section class='panel latest-vitals'>";
      html += "<div class='section-head'><h2>最新量測</h2>";
      html += "<span id='last-updated' class='last-updated'>最後更新：";
      html += latest.timestamp;
      html += "（每 3 秒刷新）</span></div>";
      html += "<p class='helper-text'><strong>複核提示：</strong>";
      html += measurementReviewLabel(review);
      html += "。";
      html += measurementReferencePolicyName();
      html += "。目前政策：";
      html += htmlEscape(String(activePolicy().policyName));
      html += " v";
      html += activePolicy().policyVersion;
      html += "。</p>";
      html += "<div class='kpi-grid'>";

      renderKpiCard(html, "kpi-sys", "pill-sys", "收縮壓", "mmHg",
                    latest.systolic, sysOk, review);
      renderKpiCard(html, "kpi-dia", "pill-dia", "舒張壓", "mmHg",
                    latest.diastolic, diaOk, review);
      renderKpiCard(html, "kpi-pul", "pill-pul", "脈搏", "bpm",
                    latest.pulse, pulOk, review);

      html += "</div><p class='helper-text'>";
      html += repeatedMeasurementGuidance();
      html += "</p><p class='helper-text'><strong>交接序號：</strong>記錄 ";
      appendUInt64(html, latest.recordSequence);
      html += "；量測工作階段 ";
      appendUInt64(html, latest.sessionSequence);
      html += "。僅將此不識別序號交給受信任的診所流程。</p></section>";

      html += "<section class='panel recent-table'>";
      html += "<div class='section-head'>";
      html += "<h2>最近 5 筆數據</h2>";
      html += "<a class='btn btn-ghost' href='/history'>查看完整歷史</a>";
      html += "</div>";
      html += "<div class='table-scroll'><table>";
      html += "<caption>最近五筆不識別量測記錄</caption><thead><tr>";
      html += "<th scope='col'>記錄序號</th><th scope='col'>測量時間</th>";
      html += "<th scope='col'>時間來源</th>";
      html += "<th scope='col'>收縮壓 (mmHg)</th><th scope='col'>舒張壓 (mmHg)</th>";
      html += "<th scope='col'>脈搏 (bpm)</th><th scope='col'>品質</th>";
      html += "<th scope='col'>複核提示</th></tr></thead><tbody>";

      const int displayCount = min(5, recordCount);
      for (int i = 0; i < displayCount; i++) {
        const BPData& record = recordManager->getRecord(i);
        html += "<tr><td>";
        appendUInt64(html, record.recordSequence);
        html += "</td><td>";
        html += record.timestamp;
        html += "</td><td>";
        html += timestampSourceCode(record.timestampSource);
        html += "</td>";
        renderTableValueCell(html, record.systolic, record.valid);
        renderTableValueCell(html, record.diastolic, record.valid);
        renderTableValueCell(html, record.pulse, record.valid);
        html += "<td>";
        html += measurementQualityCode(record.quality);
        html += record.movementCount > 0 ? "（偵測到移動）" : "（未偵測到移動）";
        html += "</td><td>";
        html += measurementReviewLabel(
          classifyMeasurement(record, activePolicy()));
        html += "</td>";
        html += "</tr>";
      }

      html += "</tbody></table></div></section>";
    } else {
      html += "<section class='panel latest-vitals'>";
      html += "<h2>最新量測</h2>";
      html += "<p class='helper-text'>尚未收到血壓數據。請先確認目前資料通道狀態，再檢查血壓機連線。</p>";
      html += "<span class='last-updated'>每 3 秒自動刷新</span>";
      html += "</section>";
    }

    html += "<details class='panel diagnostic-data' role='status' aria-live='polite'>";
    html += "<summary>接收診斷</summary>";
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
    html += "<li><span>接收診斷狀態</span><strong id='diagnostic-state'>";
    html += sanitizedDiagnosticState();
    html += "</strong></li>";
    html += "<li><span>韌體版本</span><strong>" BP_FIRMWARE_VERSION
            "（" BP_BUILD_SHA "）</strong></li>";
    html += "<li><span>支援協定</span><strong>";
    html += supportedMeasurementProtocol();
    html += "</strong></li>";
    html += "<li><span>資料遺失事件</span><strong id='data-loss-count'>";
    html += monitorTransport == nullptr ? 0U : monitorTransport->dataLossCount();
    html += "</strong></li><li><span>重新連線次數</span><strong id='reconnect-count'>";
    html += monitorTransport == nullptr ? 0U : monitorTransport->reconnectCount();
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

    if (bp_web::surfaceVisible(server->currentRole(),
                               bp_web::WebSurface::RESET_CONTROL)) {
      html += "<section class='panel danger-zone'>";
      html += "<h3>僅限管理者：維護操作</h3>";
      html += "<p class='helper-text'>若要重新配網，可重置 WiFi 設定並重啟。</p>";
      html += "<form method='post' action='/reset' onsubmit=\"return confirm('確定要重置 WiFi 設定並重啟嗎？');\" style='display:inline'>";
      html += "<button type='submit' class='btn btn-danger'>重置 WiFi 設定</button>";
      html += "</form></section>";
    }

    uint64_t initialReceiveAgeMs = 0;
    const bool hasInitialReceiveAge =
      recordManager->lastSuccessfulReceiveAgeMs(nowMs, initialReceiveAgeMs);
    html += "<script>let bpRevision='";
    appendUInt64(html, recordManager->getRevision());
    html += "';let bpPolicyVersion='";
    html += activePolicy().policyVersion;
    html += "';let bpStaleAfterMs=";
    html += activePolicy().staleAfterMs;
    html += ";let bpLastReceiveAgeMs=";
    if (hasInitialReceiveAge) {
      html += "bpBoundedAge('";
      appendUInt64(html, initialReceiveAgeMs);
      html += "')";
    } else {
      html += "null";
    }
    html += F(
      ";let bpRequestDeadlineMs=8000;"
      "let bpLastReceiveObservedAt=Date.now();"
      "let bpLastSuccessfulResponseAt=Date.now();"
      "let bpLastPollSuccess='頁面載入';"
      "let bpPollInFlight=false,bpPollTimer=0,bpWatchdogTimer=0;"
      "let bpDeadlineTimer=0,bpAbortController=null,bpTimersStopped=false;"
      "async function bpRefresh(){"
        "if(bpTimersStopped||bpPollInFlight)return;"
        "if(document.hidden){bpSchedulePoll();return;}"
        "bpPollInFlight=true;"
        "bpAbortController=new AbortController();"
        "bpDeadlineTimer=setTimeout(()=>{if(bpAbortController)bpAbortController.abort();},bpRequestDeadlineMs);"
        "try{"
        "const r=await fetch('/api/latest',{cache:'no-store',signal:bpAbortController.signal});"
        "if(!r.ok)throw new Error('poll-failure');"
        "const d=await r.json();"
        "if(String(d.policy_version)!==bpPolicyVersion){location.reload();return;}"
        "if(String(d.revision)!==bpRevision){location.reload();return;}"
        "const now=Date.now();"
        "bpLastSuccessfulResponseAt=now;"
        "bpLastReceiveAgeMs=bpBoundedAge(d.last_successful_receive_age_ms);"
        "bpLastReceiveObservedAt=now;"
        "bpLastPollSuccess=new Date().toLocaleTimeString('zh-TW',{hour12:false});"
        "const t=document.getElementById('conn-transport');if(t)t.textContent=d.transport_name;"
        "const s=document.getElementById('conn-status');if(s)s.textContent=d.transport_status;"
        "const ip=document.getElementById('conn-ip');if(ip)ip.textContent=d.wifi_ip||'未連線';"
        "const x=document.getElementById('diagnostic-state');if(x)x.textContent=d.diagnostic_state;"
        "const loss=document.getElementById('data-loss-count');if(loss)loss.textContent=d.data_loss_count;"
        "const reconnect=document.getElementById('reconnect-count');if(reconnect)reconnect.textContent=d.reconnect_count;"
        "bpFreshness(d.freshness_state,d.freshness_label,bpLastReceiveAgeMs,false);"
        "if(d.count>0){"
          "if(!document.getElementById('kpi-sys')){location.reload();return;}"
          "const v=d.valid===true;"
          "bpKpi('kpi-sys','pill-sys',d.systolic,d.review_state,d.review_label,v&&d.systolic>0);"
          "bpKpi('kpi-dia','pill-dia',d.diastolic,d.review_state,d.review_label,v&&d.diastolic>0);"
          "bpKpi('kpi-pul','pill-pul',d.pulse,d.review_state,d.review_label,v&&d.pulse>0);"
          "const u=document.getElementById('last-updated');"
          "if(u)u.textContent='最後更新：'+d.timestamp+'（每 3 秒刷新）';"
        "}else if(document.getElementById('kpi-sys')){"
          "location.reload();return;"
        "}}catch(e){"
          "bpConnectionProblem(e&&e.name==='AbortError'?'request-timeout':'poll-failure');"
        "}finally{"
          "clearTimeout(bpDeadlineTimer);bpDeadlineTimer=0;bpAbortController=null;"
          "bpPollInFlight=false;bpSchedulePoll();"
        "}"
      "}"
      "function bpSchedulePoll(){"
        "if(bpTimersStopped)return;clearTimeout(bpPollTimer);"
        "bpPollTimer=setTimeout(bpRefresh,3000);"
      "}"
      "function bpBoundedAge(value){"
        "if(value===null||value===undefined)return null;const text=String(value);"
        "if(!/^[0-9]+$/.test(text)||text.length>15)return bpStaleAfterMs;"
        "const age=Number(text);"
        "return Number.isSafeInteger(age)&&age>=0?Math.min(age,bpStaleAfterMs):bpStaleAfterMs;"
      "}"
      "function bpConnectionProblem(reason){"
        "const b=document.getElementById('measurement-freshness');"
        "if(b){b.dataset.state='disconnected';b.dataset.reason=reason;"
        "b.textContent='資料更新中斷；畫面可能過期。請檢查資料通道並重新整理。最後成功更新：'+bpLastPollSuccess;}"
        "const s=document.getElementById('conn-status');if(s)s.textContent='資料輪詢無回應';"
      "}"
      "function bpWatchdog(){"
        "if(bpTimersStopped)return;const now=Date.now();"
        "const responseAge=Math.max(0,now-bpLastSuccessfulResponseAt);"
        "const receiveAge=bpLastReceiveAgeMs===null?null:"
          "bpLastReceiveAgeMs+Math.max(0,now-bpLastReceiveObservedAt);"
        "if(responseAge>=bpRequestDeadlineMs){"
          "bpConnectionProblem('watchdog-timeout');return;"
        "}"
        "if(receiveAge!==null&&receiveAge>=bpStaleAfterMs){"
          "bpFreshness('stale','已逾時',receiveAge,true);"
        "}"
      "}"
      "function bpStopTimers(){"
        "bpTimersStopped=true;clearTimeout(bpPollTimer);clearTimeout(bpDeadlineTimer);"
        "clearInterval(bpWatchdogTimer);if(bpAbortController)bpAbortController.abort();"
      "}"
      "function bpFreshness(state,label,age,needsAction){"
        "const b=document.getElementById('measurement-freshness');if(!b)return;"
        "b.dataset.state=state;let suffix='';"
        "if(age!==null&&age!==undefined)suffix='；最後成功接收約 '+Math.floor(age/1000)+' 秒前';"
        "if(needsAction)suffix+='；請重新量測並確認資料通道';"
        "b.textContent='資料新鮮度：'+label+suffix;"
      "}"
      "function bpKpi(iv,ip,v,state,label,ok){"
        "const a=document.getElementById(iv),b=document.getElementById(ip);"
        "if(!ok){"
          "if(a){a.textContent='—';a.className='kpi-value value-na';}"
          "if(b){b.textContent='未解析';b.className='state-pill state-na';}"
          "return;"
        "}"
        "const review=state!=='within_reference';"
        "if(a){a.textContent=v;a.className='kpi-value '+(review?'value-bad':'value-good');}"
        "if(b){b.textContent=label;b.className='state-pill '+(review?'state-alert':'state-ok');}"
      "}"
      "bpWatchdogTimer=setInterval(bpWatchdog,1000);"
      "window.addEventListener('pagehide',bpStopTimers,{once:true});"
      "window.addEventListener('pageshow',(event)=>{if(event.persisted)location.reload();});"
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

    html += "<div class='table-scroll'><table>";
    html += "<caption>已保存的不識別量測歷史</caption><thead><tr>";
    html += "<th scope='col'>記錄序號</th><th scope='col'>工作階段序號</th>";
    html += "<th scope='col'>測量時間</th><th scope='col'>時間來源</th>";
    html += "<th scope='col'>收縮壓 (mmHg)</th>";
    html += "<th scope='col'>舒張壓 (mmHg)</th><th scope='col'>脈搏 (bpm)</th>";
    html += "<th scope='col'>品質</th><th scope='col'>複核提示</th></tr></thead><tbody>";

    int recordCount = recordManager->getRecordCount();
    if (recordCount > 0) {
      for (int i = 0; i < recordCount; i++) {
        const BPData& record = recordManager->getRecord(i);
        html += "<tr><td>";
        appendUInt64(html, record.recordSequence);
        html += "</td><td>";
        appendUInt64(html, record.sessionSequence);
        html += "</td><td>";
        html += record.timestamp;
        html += "</td><td>";
        html += timestampSourceCode(record.timestampSource);
        html += "</td>";
        renderTableValueCell(html, record.systolic, record.valid);
        renderTableValueCell(html, record.diastolic, record.valid);
        renderTableValueCell(html, record.pulse, record.valid);
        html += "<td>";
        html += measurementQualityCode(record.quality);
        html += record.movementCount > 0 ? "（偵測到移動）" : "（未偵測到移動）";
        html += "</td><td>";
        html += measurementReviewLabel(
          classifyMeasurement(record, activePolicy()));
        html += "</td>";
        html += "</tr>";
      }
    } else {
      html += "<tr><td colspan='9'>尚無歷史記錄</td></tr>";
    }

    html += "</tbody></table></div>";
    html += "</section>";

    if (bp_web::surfaceVisible(server->currentRole(),
                               bp_web::WebSurface::CLEAR_HISTORY_CONTROL)) {
      html += "<section class='panel danger-zone'>";
      html += "<h3>僅限管理者：危險操作</h3>";
      html += "<p class='helper-text'>此操作會清除全部歷史資料且無法復原。</p>";
      html += "<form method='post' action='/clear_history' onsubmit=\"return confirm('確定要清除所有歷史記錄嗎？');\" style='display:inline'>";
      html += "<button type='submit' class='btn btn-danger'>清除記錄</button>";
      html += "</form></section>";
    }

    html += buildPageEnd();
    server->send(200, "text/html; charset=UTF-8", html);
  }

  void handleHistoryAPI() {
    // 傳 String 進去會 copy；使用 c_str() 直接引用。在 single-thread handler
    // 內 BPData 不會被修改，pointer 安全。
    JsonDocument doc;
    setUInt64Json(doc["revision"], recordManager->getRevision());
    doc["policy_name"] = activePolicy().policyName;
    doc["policy_version"] = activePolicy().policyVersion;
    doc["firmware_version"] = BP_FIRMWARE_VERSION;
    doc["protocol"] = supportedMeasurementProtocol();
    JsonArray records = doc["records"].to<JsonArray>();

    int recordCount = recordManager->getRecordCount();
    for (int i = 0; i < recordCount; i++) {
      const BPData& record = recordManager->getRecord(i);

      JsonObject recordObj = records.add<JsonObject>();
      setUInt64Json(recordObj["record_sequence"], record.recordSequence);
      setUInt64Json(recordObj["session_sequence"], record.sessionSequence);
      recordObj["timestamp"] = record.timestamp.c_str();
      recordObj["timestamp_source"] = timestampSourceCode(record.timestampSource);
      recordObj["systolic"] = record.systolic;
      recordObj["diastolic"] = record.diastolic;
      recordObj["pulse"] = record.pulse;
      recordObj["quality"] = measurementQualityCode(record.quality);
      recordObj["movement_count"] = record.movementCount;
      recordObj["valid"] = record.valid;
      recordObj["review_state"] = measurementReviewCode(
        classifyMeasurement(record, activePolicy()));
    }

    String jsonStr;
    serializeJson(doc, jsonStr);
    server->send(200, "application/json", jsonStr);
  }

  void handleLatestAPI() {
    JsonDocument doc;
    const uint64_t nowMs = uptimeClock == nullptr ? 0 : uptimeClock->nowMs();
    const int count = recordManager->getRecordCount();
    const MeasurementFreshnessState freshness = latestFreshness(nowMs);
    doc["count"] = count;
    setUInt64Json(doc["revision"], recordManager->getRevision());
    doc["freshness_state"] = measurementFreshnessCode(freshness);
    doc["freshness_label"] = measurementFreshnessLabel(freshness);
    doc["firmware_version"] = BP_FIRMWARE_VERSION;
    doc["build_identifier"] = BP_BUILD_SHA;
    doc["protocol"] = supportedMeasurementProtocol();
    doc["reference_policy"] = measurementReferencePolicyName();
    doc["policy_name"] = activePolicy().policyName;
    doc["policy_version"] = activePolicy().policyVersion;
    doc["data_loss_count"] = monitorTransport == nullptr
      ? 0U : monitorTransport->dataLossCount();
    doc["reconnect_count"] = monitorTransport == nullptr
      ? 0U : monitorTransport->reconnectCount();
    doc["diagnostic_state"] = sanitizedDiagnosticState();
    uint64_t receiveAgeMs = 0;
    if (recordManager->lastSuccessfulReceiveAgeMs(nowMs, receiveAgeMs)) {
      setUInt64Json(doc["last_successful_receive_age_ms"], receiveAgeMs);
    } else {
      doc["last_successful_receive_age_ms"] = nullptr;
    }
    if (count > 0) {
      const BPData& latest = recordManager->getLatestRecord();
      const MeasurementReviewState review =
        classifyMeasurement(latest, activePolicy());
      setUInt64Json(doc["record_sequence"], latest.recordSequence);
      setUInt64Json(doc["session_sequence"], latest.sessionSequence);
      doc["timestamp"] = latest.timestamp.c_str();
      doc["timestamp_source"] = timestampSourceCode(latest.timestampSource);
      doc["systolic"] = latest.systolic;
      doc["diastolic"] = latest.diastolic;
      doc["pulse"] = latest.pulse;
      doc["quality"] = measurementQualityCode(latest.quality);
      doc["movement_count"] = latest.movementCount;
      doc["valid"] = latest.valid;
      doc["review_state"] = measurementReviewCode(review);
      doc["review_label"] = measurementReviewLabel(review);
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

  void handleExportCsv() {
    String csv;
    appendHistoryCsv(csv, *recordManager);
    server->sendHeader("Content-Disposition", "attachment; filename=\"bp_history.csv\"");
    server->send(200, "text/csv; charset=UTF-8", csv);
  }

  void handleClearHistory() {
    if (!recordManager->clearRecords()) {
      Serial.println("history_clear_failed");
      String errorHtml = buildPageStart("清除未完成", "/history");
      errorHtml += "<section class='panel danger-zone'>";
      errorHtml += "<h2>無法確認歷史記錄已清除</h2>";
      errorHtml += "<p class='helper-text'>儲存系統未能確認清除完成；請保留目前診斷，重新載入歷史記錄後再試一次。</p>";
      errorHtml += "</section>";
      errorHtml += buildPageEnd();
      server->send(503, "text/html; charset=UTF-8", errorHtml);
      return;
    }
    *lastData = ""; // 同步清掉舊診斷，避免 dashboard 顯示陳舊狀態

    String html = buildPageStart("記錄已清除", "/history", false, "<meta http-equiv='refresh' content='2;url=/history'>");
    html += "<section class='panel danger-zone'>";
    html += "<h2>所有歷史記錄已清除</h2>";
    html += "<p class='helper-text'>正在返回歷史記錄頁面...</p>";
    html += "</section>";
    html += buildPageEnd();

    server->send(200, "text/html; charset=UTF-8", html);
  }

};

#endif
