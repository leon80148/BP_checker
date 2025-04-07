#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "lib/BP_Parser.h"        // 引入血壓機解析器庫
#include "lib/BPRecordManager.h"  // 引入血壓記錄管理器庫

// USB接口的引腳定義
#define USB_DP_PIN 20  // USB D+ 引腳
#define USB_DN_PIN 19  // USB D- 引腳

// AP模式設定
const char* ap_ssid = "ESP32_BP_checker";
const char* ap_password = "12345678";
const char* hostname = "bp_checker"; // mDNS主機名

// WiFi設定
String sta_ssid = "";
String sta_password = "";

// 建立Web伺服器
WebServer server(80);

// 非易失性儲存
Preferences preferences;

// 血壓機型號參數
String bp_model = "OMRON-HBP9030"; // 預設型號

// 建立血壓解析器
BP_Parser bpParser("OMRON-HBP9030");

// 建立血壓記錄管理器
BP_RecordManager recordManager(20); // 保存最近20筆記錄

bool rs232_active = false;
unsigned long lastRs232Activity = 0;
String lastData = "";
bool apMode = false;

#define RESET_PIN 0  // 使用GPIO0按鈕，多數ESP32開發板上有這個按鈕

void setup() {
  // 初始化串列埠監視器
  Serial.begin(115200);
  delay(1000); // 等待串列埠穩定
  
  // 初始化 USB 用於與血壓機通訊
  Serial2.begin(9600, SERIAL_8N1, USB_DN_PIN, USB_DP_PIN);
  
  // 從非易失性記憶體讀取WiFi設定
  preferences.begin("wifi-config", false);
  sta_ssid = preferences.getString("ssid", "");
  sta_password = preferences.getString("password", "");
  
  // 讀取血壓機型號設定
  bp_model = preferences.getString("bp_model", "OMRON-HBP9030");
  
  // 設置血壓解析器型號
  bpParser.setModel(bp_model);
  
  // 從儲存中加載歷史記錄
  recordManager.loadFromStorage();
  Serial.println("已加載 " + String(recordManager.getRecordCount()) + " 筆血壓記錄");
  
  // 檢查是否有已保存的WiFi設定
  if (sta_ssid == "" || sta_password == "") {
    // 沒有設定，進入AP模式
    startAPMode();
  } else {
    // 有設定，嘗試連接WiFi (啟用雙模式)
    connectToWiFi();
  }

  pinMode(RESET_PIN, INPUT_PULLUP);
  
  // 設置NTP時間同步，使用台北時區（GMT+8）
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // 設置台北時區
  setenv("TZ", "CST-8", 1);
  tzset();
}

void startAPMode() {
  apMode = true;
  Serial.println("進入AP模式設定...");
  
  // 設置AP模式
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP地址: ");
  Serial.println(myIP);
  
  // 設置配置網頁路由
  server.on("/", HTTP_GET, handleRoot);
  server.on("/configure", HTTP_POST, handleConfigure);
  server.on("/data", HTTP_GET, []() {
    server.send(200, "text/plain", lastData);
  });
  
  // 添加重置路由
  server.on("/reset", HTTP_GET, []() {
    preferences.begin("wifi-config", false);
    preferences.clear(); // 清除整個命名空間
    preferences.end();
    
    String html = "<html><head><meta charset='UTF-8'><title>重置完成</title>";
    html += "<meta http-equiv='refresh' content='3;url=/'>";  // 3秒後重定向
    html += "<style>body{font-family:Arial;text-align:center;margin-top:50px;}</style></head>";
    html += "<body><h2>WiFi設定已重置</h2><p>裝置將重新啟動...</p></body></html>";
    server.send(200, "text/html", html);
    
    delay(1000);
    ESP.restart();
  });
  
  // 添加血壓機型號設定路由
  server.on("/bp_model", HTTP_GET, handleBpModelPage);
  server.on("/set_bp_model", HTTP_POST, handleSetBpModel);
  
  // 添加歷史記錄相關API
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/api/history", HTTP_GET, handleHistoryAPI);
  server.on("/clear_history", HTTP_GET, handleClearHistory);
  
  server.on("/raw_data", HTTP_GET, handleRawData);
  
  server.begin();
  Serial.println("HTTP伺服器已啟動");
  Serial.println("請連接到WiFi: " + String(ap_ssid) + "，密碼: " + String(ap_password));
  Serial.println("然後開啟瀏覽器訪問: " + myIP.toString());
}

void connectToWiFi() {
  Serial.println("嘗試連接到WiFi...");
  // 使用AP+STA雙模式，保持AP可訪問
  WiFi.mode(WIFI_AP_STA);
  
  // 維持AP熱點開啟
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP模式IP地址: ");
  Serial.println(apIP);
  
  // 嘗試連接到已設定的WiFi
  WiFi.begin(sta_ssid.c_str(), sta_password.c_str());
  
  // 嘗試連接WiFi，最多嘗試20秒
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n已連接到WiFi");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
    
    // 設置mDNS服務
    if(MDNS.begin(hostname)) {
      Serial.println("mDNS已啟動，可通過 http://" + String(hostname) + ".local 訪問");
    } else {
      Serial.println("mDNS服務啟動失敗");
    }
    
    // 設置監測網頁路由
    server.on("/", HTTP_GET, handleMonitor);
    server.on("/config", HTTP_GET, handleRoot); // 添加配置頁面入口
    server.on("/configure", HTTP_POST, handleConfigure);
    server.on("/data", HTTP_GET, []() {
      server.send(200, "text/plain", lastData);
    });
    
    // 添加歷史記錄相關API
    server.on("/history", HTTP_GET, handleHistory);
    server.on("/api/history", HTTP_GET, handleHistoryAPI);
    server.on("/clear_history", HTTP_GET, handleClearHistory);
    
    // 添加血壓機型號設定路由
    server.on("/bp_model", HTTP_GET, handleBpModelPage);
    server.on("/set_bp_model", HTTP_POST, handleSetBpModel);
    
    // 添加重置路由
    server.on("/reset", HTTP_GET, []() {
      preferences.begin("wifi-config", false);
      preferences.clear(); // 清除整個命名空間
      preferences.end();
      
      String html = "<html><head><meta charset='UTF-8'><title>重置完成</title>";
      html += "<meta http-equiv='refresh' content='3;url=/'>";  // 3秒後重定向
      html += "<style>body{font-family:Arial;text-align:center;margin-top:50px;}</style></head>";
      html += "<body><h2>WiFi設定已重置</h2><p>裝置將重新啟動...</p></body></html>";
      server.send(200, "text/html", html);
      
      delay(1000);
      ESP.restart();
    });
    
    server.begin();
    
    Serial.println("\n\n===== ESP32 血壓機 WiFi 轉發器 =====");
    Serial.println("設備名稱: ESP32_BP_Monitor");
    Serial.println("血壓機型號: " + bp_model);
    Serial.println("與電腦通訊: 115200 bps");
    Serial.println("與血壓機通訊: 9600 bps (RX:" + String(USB_DN_PIN) + ", TX:" + String(USB_DP_PIN) + ")");
    Serial.print("WiFi IP 地址: ");
    Serial.println(WiFi.localIP());
    Serial.println("等待數據中...");
  } else {
    Serial.println("\nWiFi連接失敗，僅使用AP模式");
    // 已經在設置AP模式，無需重新啟動AP
  }
}

void handleRoot() {
  String html = "<html><head><meta charset='UTF-8'><title>血壓機WiFi設定</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;margin:20px;} ";
  html += ".form-box{background:#f0f0f0;border:1px solid #ddd;padding:20px;border-radius:5px;max-width:400px;margin:0 auto;}";
  html += "input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;}";
  html += "button{background:#4CAF50;color:white;padding:10px;border:none;cursor:pointer;width:100%;}";
  html += ".info{background:#e8f4f8;padding:10px;border-radius:5px;margin-top:15px;font-size:14px;}";
  html += ".nav{margin-top:20px;text-align:center;}";
  html += ".nav a{margin:0 10px;color:#007bff;text-decoration:none;}";
  html += "</style></head><body>";
  html += "<div class='form-box'>";
  html += "<h2>血壓機WiFi設定</h2>";
  html += "<form method='post' action='/configure'>";
  html += "WiFi名稱:<br><input type='text' name='ssid' placeholder='輸入WiFi名稱'><br>";
  html += "WiFi密碼:<br><input type='password' name='password' placeholder='輸入WiFi密碼'><br><br>";
  html += "<button type='submit'>儲存並連接</button>";
  html += "</form>";
  
  if (WiFi.status() == WL_CONNECTED) {
    html += "<div class='info'>";
    html += "<p>目前WiFi狀態: <strong>已連接</strong></p>";
    html += "<p>IP地址: " + WiFi.localIP().toString() + "</p>";
    html += "<p>可訪問地址: <strong>http://" + String(hostname) + ".local</strong></p>";
    html += "</div>";
  }
  
  html += "<div class='nav'>";
  html += "<a href='/bp_model'>血壓機型號設定</a>";
  html += "<a href='/'>返回監控</a>";
  html += "</div>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleConfigure() {
  String new_ssid = server.arg("ssid");
  String new_password = server.arg("password");
  
  if (new_ssid.length() > 0) {
    // 儲存設定
    preferences.putString("ssid", new_ssid);
    preferences.putString("password", new_password);
    preferences.end();
    
    String html = "<html><head><meta charset='UTF-8'><title>設定完成</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial;margin:20px;text-align:center;} ";
    html += ".success-box{background:#dff0d8;border:1px solid #d6e9c6;padding:20px;border-radius:5px;max-width:400px;margin:20px auto;}";
    html += "</style></head><body>";
    html += "<div class='success-box'>";
    html += "<h2>WiFi設定已儲存</h2>";
    html += "<p>設備將重新啟動並嘗試連接到新的WiFi...</p>";
    html += "<p>連接成功後可通過以下方式訪問:</p>";
    html += "<p><strong>http://" + String(hostname) + ".local</strong></p>";
    html += "<p>或通過" + String(ap_ssid) + " WiFi熱點查看IP地址</p>";
    html += "</div></body></html>";
    
    server.send(200, "text/html", html);
    
    // 延遲2秒後重啟
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "無效的WiFi設定");
  }
}

void handleBpModelPage() {
  String html = "<html><head><meta charset='UTF-8'><title>血壓機型號設定</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;margin:20px;} ";
  html += ".form-box{background:#f0f0f0;border:1px solid #ddd;padding:20px;border-radius:5px;max-width:400px;margin:0 auto;}";
  html += "select{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;}";
  html += "button{background:#4CAF50;color:white;padding:10px;border:none;cursor:pointer;width:100%;}";
  html += ".info{background:#e8f4f8;padding:10px;border-radius:5px;margin-top:15px;font-size:14px;}";
  html += ".nav{margin-top:20px;text-align:center;}";
  html += ".nav a{margin:0 10px;color:#007bff;text-decoration:none;}";
  html += "</style></head><body>";
  html += "<div class='form-box'>";
  html += "<h2>血壓機型號設定</h2>";
  html += "<form method='post' action='/set_bp_model'>";
  html += "選擇血壓機型號:<br>";
  html += "<select name='model'>";
  html += String("<option value='OMRON-HBP9030'") + (bp_model == "OMRON-HBP9030" ? " selected" : "") + ">OMRON HBP-9030</option>";
  html += String("<option value='OMRON-HBP1300'") + (bp_model == "OMRON-HBP1300" ? " selected" : "") + ">OMRON HBP-1300</option>";
  html += String("<option value='OMRON-HEM7121'") + (bp_model == "OMRON-HEM7121" ? " selected" : "") + ">OMRON HEM-7121</option>";
  html += String("<option value='TERUMO-ES-P2020'") + (bp_model == "TERUMO-ES-P2020" ? " selected" : "") + ">TERUMO ES-P2020</option>";
  html += String("<option value='CUSTOM'") + (bp_model == "CUSTOM" ? " selected" : "") + ">自定義格式</option>";
  html += "</select><br>";
  html += "<button type='submit'>儲存設定</button>";
  html += "</form>";
  
  html += "<div class='info'>";
  html += "<p>目前血壓機型號: <strong>" + bp_model + "</strong></p>";
  html += "</div>";
  
  html += "<div class='nav'>";
  html += "<a href='/'>返回主頁</a>";
  html += "</div>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSetBpModel() {
  String new_model = server.arg("model");
  
  if (new_model.length() > 0) {
    // 儲存設定
    preferences.begin("wifi-config", false);
    preferences.putString("bp_model", new_model);
    preferences.end();
    
    bp_model = new_model;
    bpParser.setModel(bp_model); // 更新解析器模型
    
    String html = "<html><head><meta charset='UTF-8'><title>型號設定完成</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='2;url=/'>";  // 2秒後重定向
    html += "<style>body{font-family:Arial;margin:20px;text-align:center;} ";
    html += ".success-box{background:#dff0d8;border:1px solid #d6e9c6;padding:20px;border-radius:5px;max-width:400px;margin:20px auto;}";
    html += "</style></head><body>";
    html += "<div class='success-box'>";
    html += "<h2>血壓機型號已設定為: " + new_model + "</h2>";
    html += "<p>正在返回主頁...</p>";
    html += "</div></body></html>";
    
    server.send(200, "text/html", html);
  } else {
    server.send(400, "text/plain", "無效的型號設定");
  }
}

void handleMonitor() {
  String html = "<html><head><meta charset='UTF-8'>";
  html += "<title>血壓機監控</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='3'>"; // 每3秒自動刷新
  html += "<style>body{font-family:Arial;margin:20px;}";
  html += ".data-box{background:#f0f0f0;border:1px solid #ddd;padding:15px;border-radius:5px;margin-bottom:20px;}";
  html += ".header{display:flex;justify-content:space-between;align-items:center;}";
  html += ".config-btn{background:#007bff;color:white;padding:8px 12px;border:none;border-radius:4px;text-decoration:none;}";
  html += ".info{font-size:12px;color:#666;margin-top:15px;}";
  html += ".history-link{display:block;text-align:center;margin:15px 0;font-size:16px;}";
  html += ".bp-data{display:flex;flex-wrap:wrap;justify-content:center;gap:20px;margin-top:15px;}";
  html += ".bp-item{width:30%;text-align:center;padding:10px;border-radius:5px;}";
  html += ".bp-value{font-size:24px;font-weight:bold;}";
  html += ".systolic{background:#ffebee;}";
  html += ".diastolic{background:#e8f5e9;}";
  html += ".pulse{background:#e3f2fd;}";
  html += ".abnormal{color:red;}";
  html += ".normal{color:green;}";
  html += "</style></head><body>";
  html += "<div class='header'><h1>血壓機數據監控</h1>";
  html += "<div><a href='/config' class='config-btn'>WiFi設定</a> ";
  html += "<a href='/bp_model' class='config-btn'>型號設定</a></div></div>";

  // 解析後的血壓數據顯示區
  if (recordManager.getRecordCount() > 0) {
    BPData latest = recordManager.getLatestRecord();
    
    html += "<div class='data-box'>";
    html += "<h2>最新血壓數據:</h2>";
    html += "<p>測量時間: " + latest.timestamp + "</p>";
    html += "<div class='bp-data'>";
    
    // 收縮壓顯示
    html += "<div class='bp-item systolic'>";
    html += "<div>收縮壓 (mmHg)</div>";
    String systolicClass = (latest.systolic > 130 || latest.systolic < 90) ? "abnormal" : "normal";
    html += "<div class='bp-value " + systolicClass + "'>" + String(latest.systolic) + "</div>";
    html += "</div>";
    
    // 舒張壓顯示
    html += "<div class='bp-item diastolic'>";
    html += "<div>舒張壓 (mmHg)</div>";
    String diastolicClass = (latest.diastolic > 80 || latest.diastolic < 50) ? "abnormal" : "normal";
    html += "<div class='bp-value " + diastolicClass + "'>" + String(latest.diastolic) + "</div>";
    html += "</div>";
    
    // 脈搏顯示
    html += "<div class='bp-item pulse'>";
    html += "<div>脈搏 (bpm)</div>";
    String pulseClass = (latest.pulse > 100 || latest.pulse < 60) ? "abnormal" : "normal";
    html += "<div class='bp-value " + pulseClass + "'>" + String(latest.pulse) + "</div>";
    html += "</div>";
    
    html += "</div></div>";
  }
  
  // 原始數據顯示區
  html += "<div class='data-box'><h2>原始數據:</h2><div id='data'>";
  html += (lastData == "") ? "等待數據..." : lastData;
  html += "</div></div>";
  
  // 歷史記錄連結
  html += "<a href='/history' class='history-link'>查看歷史記錄 (" + String(recordManager.getRecordCount()) + "筆)</a>";
  
  html += "<div class='info'>";
  html += "<p>連接信息:</p>";
  html += "<ul>";
  html += "<li>設備名稱: BP_checker</li>";
  html += "<li>血壓機型號: " + bp_model + "</li>";
  html += "<li>IP地址: " + WiFi.localIP().toString() + "</li>";
  html += "<li>可通過 <strong>http://" + String(hostname) + ".local</strong> 訪問</li>";
  html += "<li>AP熱點: " + String(ap_ssid) + " (密碼: " + String(ap_password) + ")</li>";
  html += "</ul></div>";
  
  html += "<p><a href='/reset' style='color:red;'>重置WiFi設定</a></p>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleHistory() {
  String html = "<html><head><meta charset='UTF-8'>";
  html += "<title>血壓機歷史記錄</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;margin:20px;}";
  html += "table{width:100%;border-collapse:collapse;margin:20px 0;}";
  html += "th,td{border:1px solid #ddd;padding:12px;text-align:center;}";
  html += "th{background-color:#f2f2f2;}";
  html += "tr:nth-child(even){background-color:#f9f9f9;}";
  html += ".header{display:flex;justify-content:space-between;align-items:center;}";
  html += ".back-btn{background:#007bff;color:white;padding:8px 12px;border:none;border-radius:4px;text-decoration:none;margin-right:10px;}";
  html += ".clear-btn{background:#dc3545;color:white;padding:8px 12px;border:none;border-radius:4px;text-decoration:none;}";
  html += ".abnormal{color:red;font-weight:bold;}";
  html += ".normal{color:green;}";
  html += ".btn-group{display:flex;flex-wrap:wrap;gap:10px;}";
  html += "</style></head><body>";
  
  html += "<div class='header'>";
  html += "<h1>血壓機歷史記錄</h1>";
  html += "<div class='btn-group'>";
  html += "<a href='/' class='back-btn'>返回監控頁</a>";
  html += "<a href='/clear_history' class='clear-btn' onclick=\"return confirm('確定要清除所有歷史記錄嗎？');\">清除記錄</a>";
  html += "</div></div>";
  
  html += "<table>";
  html += "<tr><th>測量時間</th><th>收縮壓 (mmHg)</th><th>舒張壓 (mmHg)</th><th>脈搏 (bpm)</th><th>原始數據</th></tr>";
  
  // 顯示歷史記錄
  int recordCount = recordManager.getRecordCount();
  if (recordCount > 0) {
    for (int i = 0; i < recordCount; i++) {
      BPData record = recordManager.getRecord(i);
      
      html += "<tr>";
      html += "<td>" + record.timestamp + "</td>";
      
      // 收縮壓
      String systolicClass = (record.systolic > 130 || record.systolic < 90) ? "abnormal" : "normal";
      html += "<td class='" + systolicClass + "'>" + String(record.systolic) + "</td>";
      
      // 舒張壓
      String diastolicClass = (record.diastolic > 80 || record.diastolic < 50) ? "abnormal" : "normal";
      html += "<td class='" + diastolicClass + "'>" + String(record.diastolic) + "</td>";
      
      // 脈搏
      String pulseClass = (record.pulse > 100 || record.pulse < 60) ? "abnormal" : "normal";
      html += "<td class='" + pulseClass + "'>" + String(record.pulse) + "</td>";
      
      html += "<td><a href=\"/raw_data?id=" + String(i) + "\" class=\"data-link\">查看原始數據</a></td>";
      
      html += "</tr>";
    }
  } else {
    html += "<tr><td colspan='4'>尚無歷史記錄</td></tr>";
  }
  
  html += "</table>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleHistoryAPI() {
  // 使用JSON API返回歷史數據
  StaticJsonDocument<2048> doc;
  JsonArray records = doc.createNestedArray("records");
  
  int recordCount = recordManager.getRecordCount();
  for (int i = 0; i < recordCount; i++) {
    BPData record = recordManager.getRecord(i);
    
    JsonObject recordObj = records.createNestedObject();
    recordObj["timestamp"] = record.timestamp;
    recordObj["systolic"] = record.systolic;
    recordObj["diastolic"] = record.diastolic;
    recordObj["pulse"] = record.pulse;
  }
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  server.send(200, "application/json", jsonStr);
}

void handleClearHistory() {
  // 清除所有歷史記錄
  recordManager.clearRecords();
  
  String html = "<html><head><meta charset='UTF-8'><title>記錄已清除</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='2;url=/history'>";  // 2秒後重定向
  html += "<style>body{font-family:Arial;margin:20px;text-align:center;} ";
  html += ".info-box{background:#f8d7da;border:1px solid #f5c6cb;padding:20px;border-radius:5px;max-width:400px;margin:20px auto;}";
  html += "</style></head><body>";
  html += "<div class='info-box'>";
  html += "<h2>所有歷史記錄已清除</h2>";
  html += "<p>正在返回歷史記錄頁面...</p>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleRawData() {
  // 實現原始數據顯示功能
  String id = server.arg("id");
  if (id.length() > 0) {
    BPData record = recordManager.getRecord(id.toInt());
    if (record.valid) {
      server.send(200, "text/plain", record.rawData);
    } else {
      server.send(404, "text/plain", "找不到該記錄");
    }
  } else {
    server.send(400, "text/plain", "缺少記錄ID");
  }
}

void loop() {
  // 處理Web伺服器事件
  server.handleClient();
  
  // 檢測RS232活動
  if (Serial2.available()) {
    lastRs232Activity = millis();
    rs232_active = true;
    
    // 讀取數據
    uint8_t buffer[100];
    int byteCount = 0;
    String dataStr = "<h3>原始數據 (十六進制):</h3><pre>";
    
    while (Serial2.available() && byteCount < 100) {
      buffer[byteCount] = Serial2.read();
      
      // 組格式化的十六進制字串
      if (buffer[byteCount] < 0x10) dataStr += "0";
      dataStr += String(buffer[byteCount], HEX) + " ";
      
      byteCount++;
      if (byteCount % 16 == 0) dataStr += "<br>";
      delay(2);
    }
    dataStr += "</pre>";
    
    // 儲存最新數據
    lastData = dataStr;
    
    // 解析血壓數據
    BPData parsedData = bpParser.parse(buffer, byteCount);
    
    // 如果數據有效，添加到歷史記錄
    if (parsedData.valid) {
      // 獲取當前時間並格式化為台北時間
      struct tm timeinfo;
      if(getLocalTime(&timeinfo)) {
        char timeStr[64];
        // 格式化為 2025-04-05 15:30:45 格式
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        parsedData.timestamp = String(timeStr);
      } else {
        // 如果無法獲取時間，使用簡單時間戳
        parsedData.timestamp = String("時間未同步");
      }
      
      recordManager.addRecord(parsedData);
    }
    
    // 顯示數據在串列監視器
    Serial.print("接收數據: ");
    for(int i=0; i<byteCount; i++) {
      if(buffer[i] < 0x10) Serial.print("0");
      Serial.print(buffer[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    
    // 顯示解析結果
    if (parsedData.valid) {
      Serial.println("解析結果: SYS=" + String(parsedData.systolic) + " DIA=" + String(parsedData.diastolic) + " PUL=" + String(parsedData.pulse));
    } else {
      Serial.println("無法解析有效的血壓數據");
    }
    
    Serial.println("數據已準備，可通過網頁查看");
    Serial.println("----------------------------------");
  }
  
  // 如果RS232長時間無活動，設為非活動狀態
  if (rs232_active && (millis() - lastRs232Activity > 5000)) {
    rs232_active = false;
    Serial.println("RS232未檢測到活動");
  }
  
  // 檢查重置按鈕
  if (digitalRead(RESET_PIN) == LOW) {
    delay(3000);  // 長按3秒
    if (digitalRead(RESET_PIN) == LOW) {
      Serial.println("重置WiFi設定...");
      preferences.begin("wifi-config", false);
      preferences.clear();
      preferences.end();
      ESP.restart();
    }
  }
  
  delay(10);
}