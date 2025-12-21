#include "WiFiManager.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "SettingsPrefs.h"
#include "WebSerial.h"

extern Settings settings;

static DNSServer dns;
static const IPAddress apIP(192,168,4,1);

bool WiFiManager::connectSTA() {
  const char* ssid0 = settings.get.wifiSsid0();
  const char* pass0 = settings.get.wifiPass0();
  const char* ssid1 = settings.get.wifiSsid1();
  const char* pass1 = settings.get.wifiPass1();

  if (!ssid0 || !*ssid0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(settings.get.deviceName());

  // Optional static IP
  IPAddress ip, sn, gw, dnsip;
  bool useStatic = ip.fromString(settings.get.staticIP()) && ip != IPAddress(0,0,0,0);
  if (useStatic) {
    sn.fromString(settings.get.staticSN());
    gw.fromString(settings.get.staticGW());
    dnsip.fromString(settings.get.staticDNS());
    WiFi.config(ip, gw, sn, dnsip);
  }

  WiFi.begin(ssid0, pass0);

  unsigned long start = millis();
  while (millis() - start < 8000UL) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(50);
  }

  if (ssid1 && *ssid1) {
    WiFi.disconnect(true, true);
    delay(150);
    WiFi.begin(ssid1, pass1);

    start = millis();
    while (millis() - start < 8000UL) {
      if (WiFi.status() == WL_CONNECTED) return true;
      delay(50);
    }
  }

  return false;
}

void WiFiManager::startAP() {
  _apMode = true;

  WiFi.disconnect(true, true);
  delay(150);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));

  String apName = String("BambuBeacon-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.softAP(apName.c_str()); // open for now

  dns.start(53, "*", apIP);
}

void WiFiManager::begin() {
  if (!connectSTA()) startAP();
  else _apMode = false;

  if (MDNS.begin(settings.get.deviceName())) {
    MDNS.addService("http", "tcp", 80);
  }

  webSerial.printf("[WiFi] Mode=%s\n", _apMode ? "AP" : "STA");
}

void WiFiManager::loop() {
  if (_apMode) {
    dns.processNextRequest();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    _tries = 0;
    return;
  }

  const unsigned long now = millis();
  if (now - _lastTry < 15000UL) return;
  _lastTry = now;

  _tries++;
  webSerial.printf("[WiFi] Reconnect attempt %u\n", _tries);

  if (_tries >= 4) {
    webSerial.println("[WiFi] Switching to AP mode");
    startAP();
    return;
  }

  connectSTA();
}
