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
  
public:
  WiFiManager(WebServer* server, Preferences* preferences, 
              const char* ap_ssid, const char* ap_password, const char* hostname) {
    this->server = server;
    this->preferences = preferences;
    this->ap_ssid = ap_ssid;
    this->ap_password = ap_password;
    this->hostname = hostname;
    this->apMode = false;
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
    
    // 設置AP模式
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_password);
    
    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP地址: ");
    Serial.println(myIP);
    
    // 啟動網頁伺服器
    server->begin();
    
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
      
      // 啟動網頁伺服器
      server->begin();
      
      Serial.println("\n\n===== ESP32 血壓機 WiFi 轉發器 =====");
      Serial.println("設備名稱: ESP32_BP_Monitor");
      Serial.print("WiFi IP 地址: ");
      Serial.println(WiFi.localIP());
      Serial.println("等待數據中...");
    } else {
      Serial.println("\nWiFi連接失敗，僅使用AP模式");
      // 已經在設置AP模式，無需重新啟動AP
    }
  }
  
  String getStaSsid() { return sta_ssid; }
  String getStaPassword() { return sta_password; }
  bool isApMode() { return apMode; }
};

#endif 
