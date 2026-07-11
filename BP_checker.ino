#include <WiFi.h>
#include <Preferences.h>
#include <time.h>                    // configTime / tzset / setenv("TZ", ...)
#include <bootloader_random.h>
#include <esp_random.h>
// ArduinoJson、ESPmDNS 由 WebHandler.h / WiFiManager.h 已 transitively include
#include "lib/BP_Parser.h"           // 血壓機解析器
#include "lib/BPRecordManager.h"     // 血壓記錄管理器
#include "lib/WebHandler.h"          // 網頁處理器
#include "lib/WiFiManager.h"         // WiFi 管理器
#include "lib/DataProcessor.h"       // 數據處理器
#include "lib/BPConfig.h"
#include "lib/BuildInfo.h"
#include "lib/BoundedWebServer.h"
#include "lib/DeviceSecurity.h"
#include "lib/WebRequestGate.h"
#include "lib/transports/MonitorTransport.h"
#include "lib/transports/UartTransport.h"
#include "lib/transports/UsbCdcTransport.h"

// AP模式設定
const char* ap_ssid = "ESP32_BP_checker";
const char* hostname = "bp_checker"; // mDNS主機名

// 血壓機型號參數
String bp_model = "OMRON-HBP9030"; // 預設型號

// 串口通訊狀態（lastData/transportName/transportStatus 給 WebHandler 也用，所以仍是全域；
// inactivity 追蹤已封裝進 DataProcessor）
String lastData = "";
String transportName = "";
String transportStatus = "";

// 建立有固定 request/response 邊界的 Web 伺服器
bp_web::BoundedWebServer server(80);

// 非易失性儲存
Preferences preferences;

bool fillDeviceEntropy(void*, uint8_t* output, size_t length) {
  if (output == nullptr || length == 0) return false;
  esp_fill_random(output, length);
  return true;
}

DeviceEntropySource deviceEntropy = {nullptr, fillDeviceEntropy};
DeviceSecurity deviceSecurity(&preferences, deviceEntropy);
MeasurementPolicyStore measurementPolicyStore(&preferences);
bp_web::AuthFailureLimiter authFailureLimiter;
bp_web::WebRequestGate webRequestGate(&authFailureLimiter);

// 建立血壓解析器
BP_Parser bpParser("OMRON-HBP9030");

// 建立血壓記錄管理器
MonotonicMillis64 uptimeClock;
BP_RecordManager recordManager(20, &uptimeClock); // 保存最近20筆記錄

// 建立模組化管理器
WebHandler* webHandler;
WiFiManager* wifiManager;
DataProcessor* dataProcessor;
MonitorTransport* monitorTransport;
bool runtimeReady = false;

bool removePreferenceIfPresent(Preferences& store, const char* key) {
  return !store.isKey(key) || store.remove(key);
}

bool erasePendingExternalState() {
  const DeviceWipeKind wipeKind = deviceSecurity.wipeKind();
  bool applicationErased = true;
  if (preferences.begin("wifi-config", false)) {
    applicationErased = removePreferenceIfPresent(preferences, "admin_pin");
    if (wipeKind == DeviceWipeKind::NETWORK ||
        wipeKind == DeviceWipeKind::DECOMMISSION) {
      applicationErased =
        removePreferenceIfPresent(preferences, "ssid") &&
        removePreferenceIfPresent(preferences, "password") &&
        applicationErased;
    }
    preferences.end();
  } else {
    applicationErased = false;
  }

  if (wipeKind == DeviceWipeKind::DECOMMISSION &&
      !recordManager.clearRecords()) {
    applicationErased = false;
  }

  WiFi.mode(WIFI_STA);
  const bool driverErased = WiFi.eraseAP();
  (void)WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  return applicationErased && driverErased;
}

bool formatIpv4(uint32_t address, char* output, size_t capacity) {
  if (address == 0 || output == nullptr || capacity == 0) return false;
  IPAddress ip(address);
  const int written = snprintf(output, capacity, "%u.%u.%u.%u",
                               ip[0], ip[1], ip[2], ip[3]);
  return written > 0 && static_cast<size_t>(written) < capacity;
}

bool provideWebRuntimeSnapshot(
    void*, bp_web::BoundedWebRuntimeSnapshot& snapshot) {
  if (wifiManager == nullptr ||
      deviceSecurity.availability() != DeviceSecurityAvailability::READY) {
    return false;
  }
  snapshot.security.availability = deviceSecurity.availability();
  snapshot.security.claimState = deviceSecurity.claimState();
  const char* staff = deviceSecurity.secret(DeviceSecretKind::STAFF);
  const char* admin = deviceSecurity.secret(DeviceSecretKind::ADMIN);
  snapshot.security.credentials = {
    staff, strlen(staff), admin, strlen(admin)
  };

  snapshot.apActive = wifiManager->isApActive();
  snapshot.staActive = wifiManager->isStaActive();
  snapshot.apPurpose = wifiManager->apPurpose();
  snapshot.apAddress = wifiManager->apAddress();
  snapshot.staAddress = wifiManager->staAddress();
  if (snapshot.apActive &&
      !formatIpv4(snapshot.apAddress, snapshot.apHost,
                  sizeof(snapshot.apHost))) {
    return false;
  }
  if (snapshot.staActive &&
      !formatIpv4(snapshot.staAddress, snapshot.staHost,
                  sizeof(snapshot.staHost))) {
    return false;
  }
  if (snapshot.staActive) {
    const int written = snprintf(snapshot.mdnsHost,
                                 sizeof(snapshot.mdnsHost), "%s.local",
                                 wifiManager->mdnsName());
    if (written <= 0 ||
        static_cast<size_t>(written) >= sizeof(snapshot.mdnsHost)) {
      return false;
    }
  }
  return true;
}

void loadHistoryFromStorage() {
  const bool historyLoaded = recordManager.loadFromStorage();
  if (!historyLoaded) {
    lastData =
      "<div class='diagnostic-data' data-status='storage_error'>"
      "<h3>儲存診斷</h3><p><strong>狀態：</strong>storage_error</p>"
      "<p class='helper-text'>歷史記錄載入未完成；請勿將目前列表視為完整記錄。"
      "請聯絡管理人員檢查儲存空間，修復後重新啟動裝置。</p></div>";
    Serial.println("history_load_failed");
    return;
  }
  Serial.print("history_load_succeeded count=");
  Serial.println(recordManager.getRecordCount());
}

void setup() {
  // 初始化串列埠監視器
  Serial.begin(115200);
  uptimeClock.observe(static_cast<uint32_t>(millis()));
  Serial.print("BP_checker firmware ");
  Serial.print(BP_FIRMWARE_VERSION);
  Serial.print(" (");
  Serial.print(BP_BUILD_SHA);
  Serial.println(")");

  // pinMode 提早設定，讓 reset button 在後續 WiFi 連線階段也能被偵測
  pinMode(kResetPin, INPUT_PULLUP);

  // 必須在第一個 WiFi radio API 前關閉 Arduino driver persistence。
  WiFi.persistent(false);

  // ESP-IDF 要求在 RF/ADC 尚未啟用時明確打開 early-boot entropy source。
  bootloader_random_enable();
  const DeviceSecurityResult securityLoad = deviceSecurity.loadOrCreate();
  bootloader_random_disable();
  if (securityLoad != DeviceSecurityResult::OK &&
      deviceSecurity.availability() !=
        DeviceSecurityAvailability::REBOOT_REQUIRED) {
    Serial.println("security_state_load_failed");
    return;
  }
  if (deviceSecurity.availability() ==
      DeviceSecurityAvailability::REBOOT_REQUIRED) {
    ESP.restart();
    return;
  }
  if (deviceSecurity.availability() ==
      DeviceSecurityAvailability::WIPE_PENDING) {
    const bool erased = erasePendingExternalState();
    bootloader_random_enable();
    const DeviceSecurityResult eraseResult =
      deviceSecurity.finishExternalErase(erased);
    bootloader_random_disable();
    if (eraseResult != DeviceSecurityResult::OK) {
      Serial.println("security_external_erase_failed");
      if (deviceSecurity.availability() ==
          DeviceSecurityAvailability::REBOOT_REQUIRED) {
        ESP.restart();
      }
      return;
    }
  }

  if (measurementPolicyStore.loadOrCreate() != MeasurementPolicyResult::OK) {
    Serial.println("measurement_policy_load_failed");
    return;
  }

  // 初始化各個模組。先建立 transport，讓 Web operations state 直接讀取
  // main-loop 擁有的累計計數，不另複製或重設診斷狀態。
  if (kTransportMode == TRANSPORT_MODE_OTG_PRIMARY) {
    monitorTransport = new UsbCdcTransport();
  } else {
    monitorTransport = new UartTransport(&Serial1, kUartRxPin, kUartTxPin,
                                         kMonitorBaudRate);
  }

  webHandler = new WebHandler(&server, &deviceSecurity,
                              &preferences, &recordManager,
                              &bpParser,
                              &bp_model, &lastData, &transportName,
                              &transportStatus, monitorTransport,
                              &uptimeClock,
                              &measurementPolicyStore,
                              hostname, ap_ssid);

  wifiManager = new WiFiManager(
    &server, &preferences, ap_ssid,
    deviceSecurity.secret(DeviceSecretKind::AP), hostname);
  
  dataProcessor = new DataProcessor(&bpParser, &recordManager,
                                   &lastData,
                                   &transportName, &transportStatus, monitorTransport);
  
  // 舊版可能留下實驗型號；boot 也必須走 production allowlist，不能只靠 UI。
  String storedModel = "OMRON-HBP9030";
  if (preferences.begin("wifi-config", true)) {
    storedModel = preferences.getString("bp_model", "OMRON-HBP9030");
    preferences.end();
  }
  if (bp_web::isProductionModelAllowed(storedModel.c_str())) {
    bp_model = storedModel;
  } else {
    bp_model = "OMRON-HBP9030";
    Serial.println("unsupported_stored_model_using_hbp9030");
  }
  bpParser.setModel(bp_model);
  
  // 從儲存中加載歷史記錄
  loadHistoryFromStorage();
  
  // 設置網頁路由
  webHandler->setupRoutes();
  server.configureAccess(&webRequestGate, provideWebRuntimeSnapshot, nullptr);
  
  // 初始化數據處理器
  dataProcessor->setup();
  
  // 載入WiFi設定
  wifiManager->loadCredentials();
  
  // 檢查是否有已保存的WiFi設定
  if (deviceSecurity.claimState() == DeviceClaimState::UNCLAIMED) {
    wifiManager->discardLoadedCredentials();
    Serial.print("commissioning_ap_password=");
    Serial.println(deviceSecurity.secret(DeviceSecretKind::AP));
    Serial.print("commissioning_bootstrap_token=");
    Serial.println(deviceSecurity.secret(DeviceSecretKind::BOOTSTRAP));
    wifiManager->startProvisioningAP();
  } else if (wifiManager->hasCredentials()) {
    wifiManager->connectToWiFi();
  } else {
    Serial.println("network_locked_press_button_for_recovery");
  }

  // 設置NTP時間同步，使用台北時區（GMT+8）
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // 設置台北時區
  setenv("TZ", "CST-8", 1);
  tzset();
  runtimeReady = true;
}

void loop() {
  uptimeClock.observe(static_cast<uint32_t>(millis()));
  // 處理Web伺服器事件
  if (!runtimeReady) return;
  server.handleClient();

  // 處理數據接收
  dataProcessor->processIncomingData();

  // 檢查TTL串口通訊活動狀態
  dataProcessor->checkActivity();

  // 偵測 STA 上線並延遲啟動 mDNS
  wifiManager->tick(millis());

  // 非阻塞 reset button：按住 3 秒才重置，避免誤觸卡 loop
  static unsigned long resetPressStart = 0;
  static bool recoveryTriggered = false;
  if (digitalRead(kResetPin) == LOW) {
    if (resetPressStart == 0) {
      resetPressStart = millis();
    } else if (!recoveryTriggered &&
               millis() - resetPressStart >= 3000) {
      recoveryTriggered = true;
      if (deviceSecurity.claimState() == DeviceClaimState::CLAIMED) {
        (void)wifiManager->startRecoveryMode(millis());
      }
    }
  } else {
    resetPressStart = 0;
    recoveryTriggered = false;
  }
}
