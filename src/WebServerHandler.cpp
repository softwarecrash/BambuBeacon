#include "WebServerHandler.h"
#include <WiFi.h>
#include "SettingsPrefs.h"
#include "WiFiManager.h"
#include "www.h"
#include "WebSerial.h"

extern Settings settings;
extern WiFiManager wifiManager;

WebServerHandler::WebServerHandler(AsyncWebServer& s) : server(s) {}

bool WebServerHandler::isAuthorized(AsyncWebServerRequest* req) {
  // If user is empty => no auth
  if (!settings.get.webUIuser() || !*settings.get.webUIuser()) return true;
  return req->authenticate(settings.get.webUIuser(), settings.get.webUIPass());
}

void WebServerHandler::sendGz(AsyncWebServerRequest* req, const uint8_t* data, size_t len, const char* mime) {
  AsyncWebServerResponse* r = req->beginResponse(200, mime, data, len);
  r->addHeader("Content-Encoding", "gzip");
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

void WebServerHandler::handleNetlist(AsyncWebServerRequest* req) {
  int n = WiFi.scanNetworks(false, true);
  String out = "{\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i) out += ",";
    out += "{";
    out += "\"ssid\":\"" + WiFi.SSID(i) + "\"";
    out += ",\"rssi\":" + String(WiFi.RSSI(i));
    out += ",\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
    out += ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\"";
    out += "}";
  }
  out += "]}";
  req->send(200, "application/json", out);
}

void WebServerHandler::handleSubmitConfig(AsyncWebServerRequest* req) {
  auto getP = [&](const char* name) -> String {
    if (!req->hasParam(name, true)) return "";
    return req->getParam(name, true)->value();
  };

  settings.set.deviceName(getP("devicename"));

  settings.set.wifiSsid0(getP("ssid0"));
  settings.set.wifiPass0(getP("password0"));
  settings.set.wifiBssid0(getP("bssid0"));

  settings.set.wifiSsid0(getP("ssid1"));
  settings.set.wifiPass1(getP("password1"));

  settings.set.staticIP(getP("ip"));
  settings.set.staticSN(getP("subnet"));
  settings.set.staticGW(getP("gateway"));
  settings.set.staticDNS(getP("dns"));
  settings.set.webUIuser(getP("webUser"));
  settings.set.webUIPass(getP("webPass"));


  settings.save();

  req->send(200, "application/json", "{\"success\":true}");

  delay(150);
  ESP.restart();
}

void WebServerHandler::begin() {
  // Root
  server.on("/", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!isAuthorized(req)) return req->requestAuthentication();
    sendGz(req, Status_html_gz, Status_html_gz_len, Status_html_gz_mime);
  });

  // WiFi setup should always be reachable in AP mode without login
  server.on("/wifisetup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGz(req, WiFiSetup_html_gz, WiFiSetup_html_gz_len, WiFiSetup_html_gz_mime);
  });

  // webserial
  server.on("/webserial", HTTP_GET, [&](AsyncWebServerRequest* req) {
        if (!isAuthorized(req)) return req->requestAuthentication();
    sendGz(req, WebSerial_html_gz, WebSerial_html_gz_len, WebSerial_html_gz_mime);
  });

  server.on("/style.css", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, Style_css_gz, Style_css_gz_len, Style_css_gz_mime);
  });

  server.on("/backgroundCanvas.js", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, backgroundCanvas_js_gz, backgroundCanvas_js_gz_len, backgroundCanvas_js_gz_mime);
  });

  // SVG (kept)
/*   server.on("/BambuBeacon.svg", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, BambuBeacon_svg_gz, BambuBeacon_svg_gz_len, BambuBeacon_svg_gz_mime);
  }); */

  server.on("/netlist", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleNetlist(req);
  });

  server.on("/submitConfig", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleSubmitConfig(req);
  });

  server.on("/netconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    String out = "{";
    out += "\"deviceName\":\"" + String(settings.get.deviceName()) + "\"";
    out += ",\"ssid0\":\"" + String(settings.get.wifiSsid0()) + "\"";
    out += ",\"pass0\":\"" + String(settings.get.wifiPass0()) + "\"";
    out += ",\"bssid0\":\"" + String(settings.get.wifiBssid0()) + "\"";
    out += ",\"ssid1\":\"" + String(settings.get.wifiSsid1()) + "\"";
    out += ",\"pass1\":\"" + String(settings.get.wifiPass1()) + "\"";
    /* out += ",\"bssid1\":\"" + String(settings.get.wifiBssid1()) + "\""; */
    out += ",\"ip\":\"" + String(settings.get.staticIP()) + "\"";
    out += ",\"subnet\":\"" + String(settings.get.staticSN()) + "\"";
    out += ",\"gateway\":\"" + String(settings.get.staticGW()) + "\"";
    out += ",\"dns\":\"" + String(settings.get.staticDNS()) + "\"";
    out += ",\"webUser\":\"" + String(settings.get.webUIuser()) + "\"";
    out += ",\"webPass\":\"" + String(settings.get.webUIuser()) + "\"";
    out += "}";
    req->send(200, "application/json", out);
  });

  server.on("/info.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!isAuthorized(req)) return req->requestAuthentication();

    String mode = wifiManager.isApMode() ? "AP" : "STA";
    String ip = wifiManager.isApMode() ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

    String out = "{";
    out += "\"deviceName\":\"" + String(settings.get.deviceName()) + "\"";
    out += ",\"mode\":\"" + mode + "\"";
    out += ",\"ip\":\"" + ip + "\"";
    out += ",\"rssi\":" + String(rssi);
    out += "}";
    req->send(200, "application/json", out);
  });

  server.onNotFound([&](AsyncWebServerRequest* req) {
    // Nice fallback: if in AP mode, redirect everything to setup page
    if (wifiManager.isApMode()) {
      req->redirect("/wifisetup");
      return;
    }
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  webSerial.println("[WEB] Server started");
}
