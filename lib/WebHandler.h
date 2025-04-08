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
      preferences->clear(); // 清除整個命名空間
      preferences->end();
      
      String html = "<html><head><meta charset='UTF-8'><title>重置完成</title>";
      html += "<meta http-equiv='refresh' content='3;url=/'>";  // 3秒後重定向
      html += "<style>body{font-family:Arial;text-align:center;margin-top:50px;}</style></head>";
      html += "<body><h2>WiFi設定已重置</h2><p>裝置將重新啟動...</p></body></html>";
      server->send(200, "text/html", html);
      
      delay(1000);
      ESP.restart();
    });
  }

  void handleRoot() {
    // 執行WiFi掃描
    int n = WiFi.scanNetworks();
    
    String html = "<html><head><meta charset='UTF-8'><title>血壓機WiFi設定</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial;margin:20px;} ";
    html += ".form-box{background:#f0f0f0;border:1px solid #ddd;padding:20px;border-radius:5px;max-width:400px;margin:0 auto;}";
    html += "input, select{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;}";
    html += "button{background:#4CAF50;color:white;padding:10px;border:none;cursor:pointer;width:100%;}";
    html += ".info{background:#e8f4f8;padding:10px;border-radius:5px;margin-top:15px;font-size:14px;}";
    html += ".nav{margin-top:20px;text-align:center;}";
    html += ".nav a{margin:0 10px;color:#007bff;text-decoration:none;}";
    html += ".wifi-strength{font-size:12px;color:#666;}";
    html += ".refresh-btn{background:#007bff;color:white;padding:5px 10px;border:none;border-radius:3px;text-decoration:none;font-size:12px;margin-left:10px;}";
    html += "</style>";
    
    // 添加JavaScript用於手動輸入SSID
    html += "<script>";
    html += "function toggleManualSSID() {";
    html += "  var select = document.getElementById('wifi-select');";
    html += "  var manualInput = document.getElementById('manual-ssid');";
    html += "  if(select.value === 'manual') {";
    html += "    manualInput.style.display = 'block';";
    html += "    manualInput.required = true;";
    html += "  } else {";
    html += "    manualInput.style.display = 'none';";
    html += "    manualInput.required = false;";
    html += "  }";
    html += "}";
    html += "</script>";
    
    html += "</head><body>";
    html += "<div class='form-box'>";
    html += "<h2>血壓機WiFi設定</h2>";
    html += "<form method='post' action='/configure'>";
    
    // WiFi選擇下拉選單
    html += "選擇WiFi網路:<br>";
    html += "<select id='wifi-select' name='ssid' onchange='toggleManualSSID()'>";
    
    if (n == 0) {
      html += "<option value=''>找不到WiFi網路</option>";
    } else {
      for (int i = 0; i < n; ++i) {
        // 計算信號強度百分比
        int quality = 2 * (WiFi.RSSI(i) + 100);
        if (quality > 100) quality = 100;
        if (quality < 0) quality = 0;
        
        html += "<option value='" + WiFi.SSID(i) + "'>" + 
                WiFi.SSID(i) + " (" + quality + "% " +
                ((WiFi.encryptionType(i) == WIFI_AUTH_OPEN)?" 開放":"") + ")</option>";
      }
    }
    
    html += "<option value='manual'>手動輸入...</option>";
    html += "</select><br>";
    
    // 手動輸入SSID的輸入框（預設隱藏）
    html += "<input type='text' id='manual-ssid' name='manual_ssid' placeholder='輸入WiFi名稱' style='display:none'><br>";
    
    html += "WiFi密碼:<br><input type='password' name='password' placeholder='輸入WiFi密碼'><br><br>";
    html += "<button type='submit'>儲存並連接</button>";
    html += "</form>";
    
    // 刷新按鈕
    html += "<div style='text-align:right;margin-top:10px;'>";
    html += "<a href='/config' class='refresh-btn'>重新掃描WiFi</a>";
    html += "</div>";
    
    if (WiFi.status() == WL_CONNECTED) {
      html += "<div class='info'>";
      html += "<p>目前WiFi狀態: <strong>已連接</strong></p>";
      html += "<p>IP地址: " + WiFi.localIP().toString() + "</p>";
      html += "<p>可訪問地址: <strong>http://" + String(*hostname) + ".local</strong></p>";
      html += "</div>";
    }
    
    html += "<div class='nav'>";
    html += "<a href='/bp_model'>血壓機型號設定</a>";
    html += "<a href='/'>返回監控</a>";
    html += "</div>";
    
    html += "</div></body></html>";
    
    server->send(200, "text/html", html);
  }

  void handleConfigure() {
    String new_ssid;
    String new_password = server->arg("password");
    
    // 檢查是否為手動輸入的SSID
    if (server->arg("ssid") == "manual") {
      new_ssid = server->arg("manual_ssid");
    } else {
      new_ssid = server->arg("ssid");
    }
    
    if (new_ssid.length() > 0) {
      // 儲存設定
      preferences->begin("wifi-config", false);
      preferences->putString("ssid", new_ssid);
      preferences->putString("password", new_password);
      preferences->end();
      
      String html = "<html><head><meta charset='UTF-8'><title>設定完成</title>";
      html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
      html += "<style>body{font-family:Arial;margin:20px;text-align:center;} ";
      html += ".success-box{background:#dff0d8;border:1px solid #d6e9c6;padding:20px;border-radius:5px;max-width:400px;margin:20px auto;}";
      html += "</style></head><body>";
      html += "<div class='success-box'>";
      html += "<h2>WiFi設定已儲存</h2>";
      html += "<p>設備將重新啟動並嘗試連接到新的WiFi...</p>";
      html += "<p>連接成功後可通過以下方式訪問:</p>";
      html += "<p><strong>http://" + String(*hostname) + ".local</strong></p>";
      html += "<p>或通過" + String(*ap_ssid) + " WiFi熱點查看IP地址</p>";
      html += "<p>若無法順利連結，請長按Reset鍵3秒重置設定</p>";
      html += "</div></body></html>";
      
      server->send(200, "text/html", html);
      
      // 延遲2秒後重啟
      delay(2000);
      ESP.restart();
    } else {
      server->send(400, "text/plain", "無效的WiFi設定");
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
    html += String("<option value='OMRON-HBP9030'") + (*bp_model == "OMRON-HBP9030" ? " selected" : "") + ">OMRON HBP-9030</option>";
    html += String("<option value='OMRON-HBP1300'") + (*bp_model == "OMRON-HBP1300" ? " selected" : "") + ">OMRON HBP-1300</option>";
    html += String("<option value='OMRON-HEM7121'") + (*bp_model == "OMRON-HEM7121" ? " selected" : "") + ">OMRON HEM-7121</option>";
    html += String("<option value='TERUMO-ES-P2020'") + (*bp_model == "TERUMO-ES-P2020" ? " selected" : "") + ">TERUMO ES-P2020</option>";
    html += String("<option value='CUSTOM'") + (*bp_model == "CUSTOM" ? " selected" : "") + ">自定義格式</option>";
    html += "</select><br>";
    html += "<button type='submit'>儲存設定</button>";
    html += "</form>";
    
    html += "<div class='info'>";
    html += "<p>目前血壓機型號: <strong>" + *bp_model + "</strong></p>";
    html += "</div>";
    
    html += "<div class='nav'>";
    html += "<a href='/'>返回主頁</a>";
    html += "</div>";
    
    html += "</div></body></html>";
    
    server->send(200, "text/html", html);
  }

  void handleSetBpModel() {
    String new_model = server->arg("model");
    
    if (new_model.length() > 0) {
      // 儲存設定
      preferences->begin("wifi-config", false);
      preferences->putString("bp_model", new_model);
      preferences->end();
      
      *bp_model = new_model;
      
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
      
      server->send(200, "text/html", html);
    } else {
      server->send(400, "text/plain", "無效的型號設定");
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
    if (recordManager->getRecordCount() > 0) {
      BPData latest = recordManager->getLatestRecord();
      
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
    html += (*lastData == "") ? "等待數據..." : *lastData;
    html += "</div></div>";
    
    // 歷史記錄連結
    html += "<a href='/history' class='history-link'>查看歷史記錄 (" + String(recordManager->getRecordCount()) + "筆)</a>";
    
    html += "<div class='info'>";
    html += "<p>連接信息:</p>";
    html += "<ul>";
    html += "<li>設備名稱: BP_checker</li>";
    html += "<li>血壓機型號: " + *bp_model + "</li>";
    html += "<li>IP地址: " + WiFi.localIP().toString() + "</li>";
    html += "<li>可通過 <strong>http://" + String(*hostname) + ".local</strong> 訪問</li>";
    html += "<li>AP熱點: " + String(*ap_ssid) + " (密碼: " + String(*ap_password) + ")</li>";
    html += "</ul></div>";
    
    html += "<p><a href='/reset' style='color:red;'>重置WiFi設定</a></p>";
    
    html += "</body></html>";
    
    server->send(200, "text/html", html);
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
    int recordCount = recordManager->getRecordCount();
    if (recordCount > 0) {
      for (int i = 0; i < recordCount; i++) {
        BPData record = recordManager->getRecord(i);
        
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
      html += "<tr><td colspan='5'>尚無歷史記錄</td></tr>";
    }
    
    html += "</table>";
    html += "</body></html>";
    
    server->send(200, "text/html", html);
  }

  void handleHistoryAPI() {
    // 使用JSON API返回歷史數據
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
    // 清除所有歷史記錄
    recordManager->clearRecords();
    
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
    
    server->send(200, "text/html", html);
  }

  void handleRawData() {
    // 實現原始數據顯示功能
    String id = server->arg("id");
    if (id.length() > 0) {
      BPData record = recordManager->getRecord(id.toInt());
      if (record.valid) {
        String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>血壓機原始數據</title>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>";
        html += "body { font-family: 'Microsoft JhengHei', 'Microsoft YaHei', '微軟正黑體', '微软雅黑', Arial, sans-serif; margin: 20px; line-height: 1.6; }";
        html += "h1 { color: #333; border-bottom: 1px solid #eee; padding-bottom: 10px; }";
        html += "h3 { color: #555; margin-top: 20px; }";
        html += "pre { background: #f5f5f5; padding: 15px; border-radius: 5px; overflow-x: auto; font-family: monospace; font-size: 14px; line-height: 1.4; }";
        html += ".back-btn { display: inline-block; margin: 20px 0; background: #007bff; color: white; padding: 8px 15px; border: none; border-radius: 4px; text-decoration: none; }";
        html += ".back-btn:hover { background: #0069d9; }";
        html += ".data-section { margin-bottom: 25px; }";
        html += "</style></head><body>";
        html += "<h1>血壓機原始數據</h1>";
        html += record.rawData;
        html += "<a href='/history' class='back-btn'>返回歷史記錄</a>";
        html += "</body></html>";
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