#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>                    // configTime / tzset / setenv("TZ", ...)
// ArduinoJson、ESPmDNS 由 WebHandler.h / WiFiManager.h 已 transitively include
#include "lib/BP_Parser.h"           // 血壓機解析器
#include "lib/BPRecordManager.h"     // 血壓記錄管理器
#include "lib/WebHandler.h"          // 網頁處理器
#include "lib/WiFiManager.h"         // WiFi 管理器
#include "lib/DataProcessor.h"       // 數據處理器
#include "lib/BPConfig.h"
#include "lib/transports/MonitorTransport.h"
#include "lib/transports/UartTransport.h"
#include "lib/transports/UsbCdcTransport.h"

// AP模式設定
const char* ap_ssid = "ESP32_BP_checker";
const char* ap_password = "12345678";
const char* hostname = "bp_checker"; // mDNS主機名

// 血壓機型號參數
String bp_model = "OMRON-HBP9030"; // 預設型號

// 串口通訊狀態（lastData/transportName/transportStatus 給 WebHandler 也用，所以仍是全域；
// inactivity 追蹤已封裝進 DataProcessor）
String lastData = "";
String transportName = "";
String transportStatus = "";

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
MonitorTransport* monitorTransport;

void setup() {
  // 初始化串列埠監視器
  Serial.begin(115200);
  delay(1000); // 等待串列埠穩定

  // pinMode 提早設定，讓 reset button 在後續 WiFi 連線階段也能被偵測
  pinMode(kResetPin, INPUT_PULLUP);

  // 初始化各個模組
  webHandler = new WebHandler(&server, &preferences, &recordManager,
                             &bpParser,
                             &bp_model, &lastData, &transportName, &transportStatus,
                             hostname, ap_ssid);
  
  wifiManager = new WiFiManager(&server, &preferences, 
                               ap_ssid, ap_password, hostname);

  if (kTransportMode == TRANSPORT_MODE_OTG_PRIMARY) {
    monitorTransport = new UsbCdcTransport();
  } else {
    monitorTransport = new UartTransport(&Serial1, kUartRxPin, kUartTxPin, kMonitorBaudRate);
  }
  
  dataProcessor = new DataProcessor(&bpParser, &recordManager,
                                   &lastData,
                                   &transportName, &transportStatus, monitorTransport);
  
  // 設置血壓解析器型號（read-only 開啟，省 NVS 寫入準備工作）
  preferences.begin("wifi-config", true);
  bp_model = preferences.getString("bp_model", "OMRON-HBP9030");
  preferences.end();
  bpParser.setModel(bp_model);
  
  // 從儲存中加載歷史記錄
  recordManager.loadFromStorage();
  Serial.print("已加載 ");
  Serial.print(recordManager.getRecordCount());
  Serial.println(" 筆血壓記錄");
  
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

  // 偵測 STA 上線並延遲啟動 mDNS
  wifiManager->tick();

  // 非阻塞 reset button：按住 3 秒才重置，避免誤觸卡 loop
  // 只清 ssid/password，保留 bp_model 等其他設定（與 UI 標籤「重置 WiFi 設定」一致）
  static unsigned long resetPressStart = 0;
  if (digitalRead(kResetPin) == LOW) {
    if (resetPressStart == 0) {
      resetPressStart = millis();
    } else if (millis() - resetPressStart >= 3000) {
      Serial.println("重置WiFi設定...");
      preferences.begin("wifi-config", false);
      preferences.remove("ssid");
      preferences.remove("password");
      preferences.end();
      ESP.restart();
    }
  } else {
    resetPressStart = 0;
  }

  delay(10);
}
