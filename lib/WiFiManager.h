#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "WebHandler.h"

class WiFiManager {
private:
  WebServer* server;
  Preferences* preferences;
  WebHandler* webHandler;

  const char* ap_ssid;
  const char* ap_password;
  const char* hostname;

  String sta_ssid;
  String sta_password;

  bool apMode;
  bool serverStarted;
  bool mdnsStarted;

public:
  WiFiManager(WebServer* server, Preferences* preferences,
              const char* ap_ssid, const char* ap_password, const char* hostname) {
    this->server = server;
    this->preferences = preferences;
    this->ap_ssid = ap_ssid;
    this->ap_password = ap_password;
    this->hostname = hostname;
    this->apMode = false;
    this->serverStarted = false;
    this->mdnsStarted = false;
    this->webHandler = nullptr;
  }

  void setWebHandler(WebHandler* webHandler) {
    this->webHandler = webHandler;
  }

  void loadCredentials() {
    preferences->begin("wifi-config", false);
    sta_ssid = preferences->getString("ssid", "");
    sta_password = preferences->getString("password", "");
    preferences->end();
  }

  bool hasCredentials() {
    return sta_ssid != "";
  }

  void startAPMode() {
    apMode = true;
    Serial.println("進入AP模式設定...");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_password);

    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP地址: ");
    Serial.println(myIP);

    startServerOnce();

    Serial.println("HTTP伺服器已啟動");
    Serial.println("請連接到WiFi: " + String(ap_ssid) + "，密碼: " + String(ap_password));
    Serial.println("然後開啟瀏覽器訪問: " + myIP.toString());
  }

  // 啟動 STA 連線（不阻塞）；webserver 立即上線，
  // 連線完成後 tick() 會啟動 mDNS。
  void connectToWiFi() {
    Serial.println("嘗試連接到WiFi（背景）...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ap_ssid, ap_password);
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

  // 主 loop 持續呼叫；偵測 STA 連線狀態變化並啟動 mDNS。
  void tick() {
    if (mdnsStarted) return;
    if (WiFi.status() != WL_CONNECTED) return;

    Serial.println("已連接到WiFi");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin(hostname)) {
      Serial.println("mDNS已啟動，可通過 http://" + String(hostname) + ".local 訪問");
    } else {
      Serial.println("mDNS服務啟動失敗");
    }
    mdnsStarted = true;

    Serial.println("\n===== ESP32 血壓機 WiFi 轉發器 =====");
    Serial.println("設備名稱: ESP32_BP_Monitor");
    Serial.print("WiFi IP 地址: ");
    Serial.println(WiFi.localIP());
    Serial.println("等待數據中...");
  }

  String getStaSsid() { return sta_ssid; }
  String getStaPassword() { return sta_password; }
  bool isApMode() { return apMode; }

private:
  void startServerOnce() {
    if (serverStarted) return;
    server->begin();
    serverStarted = true;
  }
};

#endif 
