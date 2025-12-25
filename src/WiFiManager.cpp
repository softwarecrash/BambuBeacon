#include "WiFiManager.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "SettingsPrefs.h"
#include "WebSerial.h"

extern Settings settings;

static DNSServer dns;
static const IPAddress apIP(192,168,4,1);

void WiFiManager::startConnectAttempt() {
  const char* ssid0 = settings.get.wifiSsid0();
  const char* pass0 = settings.get.wifiPass0();

  if (!ssid0 || !*ssid0) {
    _connectPhase = ConnectPhase::IDLE;
    return;
  }

  _lastFailNoAp = false;
  WiFi.mode(_apMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(settings.get.deviceName());

  // Optional static IP
  IPAddress ip, sn, gw, dnsip;
  const bool ipOk = ip.fromString(settings.get.staticIP()) && ip != IPAddress(0,0,0,0);
  const bool snOk = sn.fromString(settings.get.staticSN()) && sn != IPAddress(0,0,0,0);
  const bool gwOk = gw.fromString(settings.get.staticGW()) && gw != IPAddress(0,0,0,0);
  const bool dnsOk = dnsip.fromString(settings.get.staticDNS()) && dnsip != IPAddress(0,0,0,0);
  const bool useStatic = ipOk && snOk && gwOk && dnsOk;
  if (useStatic) {
    WiFi.config(ip, gw, sn, dnsip);
  }

  WiFi.begin(ssid0, pass0);
  _connectPhase = ConnectPhase::SSID0;
  _connectStart = millis();
}

WiFiManager::AttemptResult WiFiManager::processConnectAttempt() {
  if (_connectPhase == ConnectPhase::IDLE) return AttemptResult::InProgress;
  if (WiFi.status() == WL_CONNECTED) {
    _connectPhase = ConnectPhase::IDLE;
    return AttemptResult::Connected;
  }

  const unsigned long now = millis();
  const wl_status_t st = WiFi.status();
  if ((st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) &&
      (now - _connectStart) >= kFastFailNoApMs) {
    _lastFailNoAp = true;
    _connectStart = 0;
  }

  if (_connectStart == 0) {
    if (_connectPhase == ConnectPhase::SSID0) {
      const char* ssid1 = settings.get.wifiSsid1();
      const char* pass1 = settings.get.wifiPass1();
      if (ssid1 && *ssid1) {
        WiFi.begin(ssid1, pass1);
        _connectPhase = ConnectPhase::SSID1;
        _connectStart = now;
        return AttemptResult::InProgress;
      }
    }

    _connectPhase = ConnectPhase::IDLE;
    return AttemptResult::Failed;
  }

  if (now - _connectStart < kConnectTimeoutMs) return AttemptResult::InProgress;

  if (_connectPhase == ConnectPhase::SSID0) {
    const char* ssid1 = settings.get.wifiSsid1();
    const char* pass1 = settings.get.wifiPass1();
    if (ssid1 && *ssid1) {
      WiFi.begin(ssid1, pass1);
      _connectPhase = ConnectPhase::SSID1;
      _connectStart = now;
      return AttemptResult::InProgress;
    }
  }

  _connectPhase = ConnectPhase::IDLE;
  return AttemptResult::Failed;
}

void WiFiManager::startAP() {
  _apMode = true;
  _connectPhase = ConnectPhase::IDLE;
  _connectStart = 0;

  WiFi.disconnect(true, true);
  delay(150);

  // IMPORTANT: AP+STA mode enables stable WiFi scanning while AP is active
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));

  String apName = String("BambuBeacon-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.softAP(apName.c_str()); // open for now

  dns.start(53, "*", apIP);

  // Kick an async scan early so the setup page can show networks immediately
  WiFi.scanDelete();
  WiFi.scanNetworks(true /* async */, true /* show hidden */);

  // Optional pre-warm: wait briefly for the first scan results (not in AsyncTCP context)
  const unsigned long t0 = millis();
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && (millis() - t0) < 2500UL) {
    delay(20);
  }
}

void WiFiManager::begin() {
  _apMode = false;
  _tries = 0;
  _lastTry = 0;
  _connectPhase = ConnectPhase::IDLE;
  _lastFailNoAp = false;

  const char* ssid0 = settings.get.wifiSsid0();
  if (ssid0 && *ssid0) startConnectAttempt();
  else startAP();

  if (MDNS.begin(settings.get.deviceName())) {
    MDNS.addService("http", "tcp", 80);
  }

  webSerial.printf("[WiFi] Mode=%s\n", _apMode ? "AP" : "STA");
}

void WiFiManager::loop() {
  if (_apMode) {
    dns.processNextRequest();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (_apMode && WiFi.localIP() != IPAddress(0,0,0,0)) {
      webSerial.println("[WiFi] Connected in AP mode, stopping AP");
      stopAP();
    }
    _connectPhase = ConnectPhase::IDLE;
    _tries = 0;
    _lastFailNoAp = false;
    return;
  }

  const char* ssid0 = settings.get.wifiSsid0();
  const bool hasSsid0 = (ssid0 && *ssid0);
  if (!hasSsid0) {
    if (!_apMode) startAP();
    return;
  }

  const unsigned long now = millis();
  if (_apMode && _lastFailNoAp && (now - _lastTry) < kApRetryIntervalMs) {
    return;
  }
  if (_connectPhase != ConnectPhase::IDLE) {
    AttemptResult res = processConnectAttempt();
    if (res == AttemptResult::Connected) {
      _tries = 0;
      _lastFailNoAp = false;
      return;
    }
    if (res == AttemptResult::Failed) {
      _tries++;
      _lastTry = now;
      webSerial.printf("[WiFi] Reconnect attempt %u failed\n", _tries);
      if (!_apMode && (_lastFailNoAp || _tries >= kMaxTriesBeforeAp)) {
        webSerial.println("[WiFi] Switching to AP mode");
        startAP();
      }
    }
    return;
  }

  const unsigned long retryInterval = _apMode ? kApRetryIntervalMs : kRetryIntervalMs;
  if (now - _lastTry < retryInterval) return;
  if (_apMode && WiFi.softAPgetStationNum() > 0) return;
  _lastTry = now;

  if (!_apMode && _tries >= kMaxTriesBeforeAp) {
    webSerial.println("[WiFi] Switching to AP mode");
    startAP();
    return;
  }

  webSerial.printf("[WiFi] Reconnect attempt %u\n", _tries + 1);
  startConnectAttempt();
}

void WiFiManager::stopAP() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  _apMode = false;
}
