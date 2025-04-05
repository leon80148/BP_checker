#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// ESP32-S3 的 UART2 引腳定義
#define RX_PIN 16  // UART2 RX 引腳
#define TX_PIN 17  // UART2 TX 引腳

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

bool rs232_active = false;
unsigned long lastRs232Activity = 0;
String lastData = "";
bool apMode = false;

#define RESET_PIN 0  // 使用GPIO0按鈕，多數ESP32開發板上有這個按鈕

void setup() {
  // 初始化串列埠監視器
  Serial.begin(115200);
  delay(1000); // 等待串列埠穩定
  
  // 初始化 UART2 用於與血壓機通訊
  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  
  // 從非易失性記憶體讀取WiFi設定
  preferences.begin("wifi-config", false);
  sta_ssid = preferences.getString("ssid", "");
  sta_password = preferences.getString("password", "");
  
  // 檢查是否有已保存的WiFi設定
  if (sta_ssid == "" || sta_password == "") {
    // 沒有設定，進入AP模式
    startAPMode();
  } else {
    // 有設定，嘗試連接WiFi (啟用雙模式)
    connectToWiFi();
  }

  pinMode(RESET_PIN, INPUT_PULLUP);
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
    Serial.println("與電腦通訊: 115200 bps");
    Serial.println("與血壓機通訊: 9600 bps (RX:" + String(RX_PIN) + ", TX:" + String(TX_PIN) + ")");
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

void handleMonitor() {
  String html = "<html><head><meta charset='UTF-8'>";
  html += "<title>血壓機監控</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='3'>"; // 每3秒自動刷新
  html += "<style>body{font-family:Arial;margin:20px;}";
  html += ".data-box{background:#f0f0f0;border:1px solid #ddd;padding:15px;border-radius:5px;}";
  html += ".header{display:flex;justify-content:space-between;align-items:center;}";
  html += ".config-btn{background:#007bff;color:white;padding:8px 12px;border:none;border-radius:4px;text-decoration:none;}";
  html += ".info{font-size:12px;color:#666;margin-top:15px;}";
  html += "</style></head><body>";
  html += "<div class='header'><h1>血壓機數據監控</h1>";
  html += "<a href='/config' class='config-btn'>WiFi設定</a></div>";
  html += "<div class='data-box'><h2>血壓數據:</h2><div id='data'>";
  html += (lastData == "") ? "等待數據..." : lastData;
  html += "</div></div>";
  
  html += "<div class='info'>";
  html += "<p>連接信息:</p>";
  html += "<ul>";
  html += "<li>設備名稱: BP_checker</li>";
  html += "<li>IP地址: " + WiFi.localIP().toString() + "</li>";
  html += "<li>可通過 <strong>http://" + String(hostname) + ".local</strong> 訪問</li>";
  html += "<li>AP熱點: " + String(ap_ssid) + " (密碼: " + String(ap_password) + ")</li>";
  html += "</ul></div>";
  
  html += "<p><a href='/reset' style='color:red;'>重置WiFi設定</a></p>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
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
    
    // 顯示數據在串列監視器
    Serial.print("接收數據: ");
    for(int i=0; i<byteCount; i++) {
      if(buffer[i] < 0x10) Serial.print("0");
      Serial.print(buffer[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    
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