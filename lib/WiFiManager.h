#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>
#include <Preferences.h>
#include <ESPmDNS.h>

#include <cstring>

#include "BoundedWebServer.h"
#include "NetworkLifecycle.h"

class WiFiManager {
public:
  static constexpr uint32_t kRecoveryWindowMs =
    bp_network::NetworkLifecycle::kRecoveryWindowMs;

  WiFiManager(bp_web::BoundedWebServer* server, Preferences* preferences,
              const char* apSsid, const char* apPassword,
              const char* hostname)
    : server(server),
      preferences(preferences),
      apSsid(apSsid),
      apPassword(apPassword),
      hostname(hostname) {}

  void loadCredentials() {
    secureWipeString(staSsid);
    secureWipeString(staPassword);
    if (preferences == nullptr ||
        !preferences->begin("wifi-config", true)) {
      return;
    }
    staSsid = preferences->getString("ssid", "");
    staPassword = preferences->getString("password", "");
    preferences->end();
    if (staSsid.isEmpty()) secureWipeString(staPassword);
  }

  bool hasCredentials() const { return !staSsid.isEmpty(); }

  void discardLoadedCredentials() {
    secureWipeString(staSsid);
    secureWipeString(staPassword);
  }

  void startProvisioningAP() {
    if (!validApConfiguration()) return;
    const bool modeReady = WiFi.mode(WIFI_AP);
    apActive = modeReady && WiFi.softAP(apSsid, apPassword);
    if (!apActive) {
      (void)WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      lifecycle.startLocked();
      Serial.println("provisioning_ap_start_failed");
      return;
    }
    lifecycle.startProvisioning();
    startServerOnce();
    Serial.print("provisioning_ap_ready ssid=");
    Serial.println(apSsid);
    Serial.print("provisioning_url=http://");
    Serial.println(WiFi.softAPIP());
  }

  void connectToWiFi() {
    if (!hasCredentials()) return;
    closeAP();
    lifecycle.startStaOnly();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(staSsid.c_str(), staPassword.c_str());
    secureWipeString(staPassword);
    startServerOnce();
    Serial.println("sta_connecting");
  }

  bool startRecoveryMode(uint32_t nowMs) {
    if (!validApConfiguration() ||
        !lifecycle.beginRecovery(true, hasCredentials(), nowMs)) {
      return false;
    }
    const bool credentialsAvailable = hasCredentials();
    const bool modeReady = WiFi.mode(
      credentialsAvailable ? WIFI_AP_STA : WIFI_AP);
    if (!modeReady || !WiFi.softAP(apSsid, apPassword)) {
      (void)WiFi.softAPdisconnect(true);
      apActive = false;
      if (credentialsAvailable) {
        lifecycle.startStaOnly();
        WiFi.mode(WIFI_STA);
      } else {
        lifecycle.startLocked();
        WiFi.mode(WIFI_OFF);
      }
      Serial.println("recovery_ap_start_failed");
      return false;
    }
    apActive = true;
    startServerOnce();
    Serial.println("recovery_ap_ready duration_seconds=600");
    return true;
  }

  void tick(uint32_t nowMs) {
    if (lifecycle.tick(nowMs)) {
      closeAP();
      if (lifecycle.phase() == bp_network::NetworkPhase::STA_ONLY) {
        WiFi.mode(WIFI_STA);
      } else {
        WiFi.mode(WIFI_OFF);
      }
      Serial.println("recovery_ap_closed");
    }

    const bool nowConnected = WiFi.status() == WL_CONNECTED;
    lifecycle.observeStaConnected(nowConnected);
    if (nowConnected == wasConnected) return;
    wasConnected = nowConnected;
    if (!nowConnected) {
      if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
      }
      Serial.println("sta_disconnected_background_reconnect");
      return;
    }

    if (mdnsStarted) MDNS.end();
    mdnsStarted = MDNS.begin(hostname);
    Serial.print("sta_ready ip=");
    Serial.println(WiFi.localIP());
  }

  bool isApActive() const { return apActive && lifecycle.apRequired(); }
  bp_web::ApPurpose apPurpose() const {
    if (lifecycle.phase() == bp_network::NetworkPhase::PROVISIONING_AP) {
      return bp_web::ApPurpose::PROVISIONING;
    }
    if (lifecycle.phase() == bp_network::NetworkPhase::RECOVERY_AP) {
      return bp_web::ApPurpose::RECOVERY;
    }
    return bp_web::ApPurpose::NONE;
  }
  bool isStaActive() const { return WiFi.status() == WL_CONNECTED; }
  uint32_t apAddress() const {
    return apActive ? static_cast<uint32_t>(WiFi.softAPIP()) : 0;
  }
  uint32_t staAddress() const {
    return isStaActive() ? static_cast<uint32_t>(WiFi.localIP()) : 0;
  }
  const char* mdnsName() const { return hostname; }

private:
  bp_web::BoundedWebServer* server = nullptr;
  Preferences* preferences = nullptr;
  const char* apSsid = nullptr;
  const char* apPassword = nullptr;
  const char* hostname = nullptr;
  String staSsid;
  String staPassword;
  bool serverStarted = false;
  bool mdnsStarted = false;
  bool wasConnected = false;
  bool apActive = false;
  bp_network::NetworkLifecycle lifecycle;

  static void secureWipeString(String& value) {
    volatile char* bytes = value.begin();
    size_t length = value.length();
    while (length-- != 0) *bytes++ = 0;
    value = String();
  }

  bool validApConfiguration() const {
    return server != nullptr && apSsid != nullptr && apSsid[0] != '\0' &&
           apPassword != nullptr && strlen(apPassword) ==
             bp_web::kCanonicalCredentialChars;
  }

  void closeAP() {
    if (apActive) (void)WiFi.softAPdisconnect(true);
    apActive = false;
  }

  void startServerOnce() {
    if (serverStarted || server == nullptr) return;
    server->begin();
    serverStarted = true;
  }
};

#endif
