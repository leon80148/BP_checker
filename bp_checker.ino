#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "lib/BP_Parser.h"           // 引入血壓機解析器庫
#include "lib/BPRecordManager.h"     // 引入血壓記錄管理器庫
#include "lib/WebHandler.h"          // 引入網頁處理器庫
#include "lib/WiFiManager.h"         // 引入WiFi管理器庫
#include "lib/DataProcessor.h"       // 引入數據處理器庫

// TTL串口的引腳定義 (UART)
#define TTL_RX_PIN 44  // UART RX 引腳
#define TTL_TX_PIN 43  // UART TX 引腳

// AP模式設定
const char* ap_ssid = "ESP32_BP_checker";
const char* ap_password = "12345678";
const char* hostname = "bp_checker"; // mDNS主機名

// 血壓機型號參數
String bp_model = "OMRON-HBP9030"; // 預設型號

// 串口通訊狀態
bool serial_active = false;
unsigned long lastSerialActivity = 0;
String lastData = "";

// 建立Web伺服器
WebServer server(80);

// 非易失性儲存
Preferences preferences;

// 建立血壓解析器
BP_Parser bpParser("OMRON-HBP9030");

// 建立血壓記錄管理器
BP_RecordManager recordManager(20); // 保存最近20筆記錄

// 建立模組化管理器
WebHandler* webHandler;
WiFiManager* wifiManager;
DataProcessor* dataProcessor;

#define RESET_PIN 0  // 使用GPIO0按鈕，多數ESP32開發板上有這個按鈕

void setup() {
  // 初始化串列埠監視器
  Serial.begin(115200);
  delay(1000); // 等待串列埠穩定
  
  // 初始化各個模組
  webHandler = new WebHandler(&server, &preferences, &recordManager, 
                             &bp_model, &lastData, &hostname, &ap_ssid, &ap_password);
  
  wifiManager = new WiFiManager(&server, &preferences, 
                               ap_ssid, ap_password, hostname);
  
  dataProcessor = new DataProcessor(&bpParser, &recordManager, 
                                   &lastData, &serial_active, &lastSerialActivity,
                                   TTL_RX_PIN, TTL_TX_PIN);
  
  // 設置血壓解析器型號
  preferences.begin("wifi-config", false);
  bp_model = preferences.getString("bp_model", "OMRON-HBP9030");
  preferences.end();
  bpParser.setModel(bp_model);
  
  // 從儲存中加載歷史記錄
  recordManager.loadFromStorage();
  Serial.println("已加載 " + String(recordManager.getRecordCount()) + " 筆血壓記錄");
  
  // 設置網頁路由
  webHandler->setupRoutes();
  
  // 初始化數據處理器
  dataProcessor->setup();
  
  // 載入WiFi設定
  wifiManager->loadCredentials();
  
  // 檢查是否有已保存的WiFi設定
  if (!wifiManager->hasCredentials()) {
    // 沒有設定，進入AP模式
    wifiManager->startAPMode();
  } else {
    // 有設定，嘗試連接WiFi (啟用雙模式)
    wifiManager->connectToWiFi();
  }

  pinMode(RESET_PIN, INPUT_PULLUP);
  
  // 設置NTP時間同步，使用台北時區（GMT+8）
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // 設置台北時區
  setenv("TZ", "CST-8", 1);
  tzset();
}

void loop() {
  // 處理Web伺服器事件
  server.handleClient();
  
  // 處理數據接收
  dataProcessor->processIncomingData();
  
  // 檢查TTL串口通訊活動狀態
  dataProcessor->checkActivity();
  
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