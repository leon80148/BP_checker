#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

class WiFiManager {
private:
  WebServer* server;
  Preferences* preferences;

  const char* ap_ssid;
  const char* ap_password;
  const char* hostname;

  String sta_ssid;
  String sta_password;

  bool serverStarted;
  bool mdnsStarted;
  bool wasConnected;

public:
  WiFiManager(WebServer* server, Preferences* preferences,
              const char* ap_ssid, const char* ap_password, const char* hostname) {
    this->server = server;
    this->preferences = preferences;
    this->ap_ssid = ap_ssid;
    this->ap_password = ap_password;
    this->hostname = hostname;
    this->serverStarted = false;
    this->mdnsStarted = false;
    this->wasConnected = false;
  }

  void loadCredentials() {
    preferences->begin("wifi-config", true); // read-only
    sta_ssid = preferences->getString("ssid", "");
    sta_password = preferences->getString("password", "");
    preferences->end();
  }

  bool hasCredentials() {
    return sta_ssid != "";
  }

  void startAPMode() {
    Serial.println("進入AP模式設定...");

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(ap_ssid, ap_password)) {
      Serial.println("[ERROR] WiFi.softAP 失敗：AP 可能無法被連到");
    }

    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP地址: ");
    Serial.println(myIP);

    startServerOnce();

    Serial.println("HTTP伺服器已啟動");
    Serial.print("請連接到WiFi: ");
    Serial.print(ap_ssid);
    Serial.print("，密碼: ");
    Serial.println(ap_password);
    Serial.print("然後開啟瀏覽器訪問: ");
    Serial.println(myIP);
  }

  // 啟動 STA 連線（不阻塞）；webserver 立即上線，
  // 連線完成後 tick() 會啟動 mDNS。
  void connectToWiFi() {
    Serial.println("嘗試連接到WiFi（背景）...");
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(ap_ssid, ap_password)) {
      Serial.println("[ERROR] WiFi.softAP 失敗：AP 備援可能無法被連到");
    }
    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP IP地址: ");
    Serial.println(apIP);

    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.begin(sta_ssid.c_str(), sta_password.c_str());

    // 短暫等待 STA 即時連線（最多 ~2s），讓常見快速連線情境能在開機時就好
    for (int i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) {
      delay(200);
      Serial.print(".");
    }

    // 不論連線是否成功，webserver 與 AP 都立即可用
    startServerOnce();
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      tick(); // 觸發 mDNS / log
    } else {
      Serial.println("STA 尚未連上，背景持續嘗試；AP/webserver 已上線。");
    }
  }

  // 主 loop 持續呼叫；偵測 STA 連線狀態邊緣（disconnect→connect）以重新註冊 mDNS。
  void tick() {
    bool nowConnected = (WiFi.status() == WL_CONNECTED);
    if (nowConnected == wasConnected) return; // 沒邊緣

    if (nowConnected) {
      Serial.println("WiFi 連線就緒");
      Serial.print("IP地址: ");
      Serial.println(WiFi.localIP());

      if (mdnsStarted) {
        MDNS.end();
      }
      if (MDNS.begin(hostname)) {
        Serial.print("mDNS已啟動，可通過 http://");
        Serial.print(hostname);
        Serial.println(".local 訪問");
        mdnsStarted = true;
      } else {
        Serial.println("mDNS服務啟動失敗");
        mdnsStarted = false;
      }

      Serial.println("\n===== ESP32 血壓機 WiFi 轉發器 =====");
      Serial.println("設備名稱: ESP32_BP_Monitor");
      Serial.print("WiFi IP 地址: ");
      Serial.println(WiFi.localIP());
      Serial.println("等待數據中...");
    } else {
      Serial.println("WiFi STA 已斷線，背景重連中（AP/webserver 仍可用）");
      // 不立即 MDNS.end()；重連時會 end+begin
    }
    wasConnected = nowConnected;
  }

private:
  void startServerOnce() {
    if (serverStarted) return;
    server->begin();
    serverStarted = true;
  }
};

#endif 
