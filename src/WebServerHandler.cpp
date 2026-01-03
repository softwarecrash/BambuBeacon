#include "WebServerHandler.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_timer.h>
#include <Update.h>
#include "SettingsPrefs.h"
#include "WiFiManager.h"
#include "www.h"
#include "WebSerial.h"
#include "bblPrinterDiscovery.h"
#include "BambuMqttClient.h"
#include "LedController.h"
#include "GitHubOtaUpdater.h"

extern Settings settings;
extern WiFiManager wifiManager;
extern BBLPrinterDiscovery printerDiscovery;
extern BambuMqttClient bambu;
extern LedController ledsCtrl;
extern GitHubOtaUpdater ota;
static void scheduleRestart(uint32_t delayMs);

const uint8_t* webserialHtml() {
  return WebSerial_html_gz;
}

size_t webserialHtmlLen() {
  return WebSerial_html_gz_len;
}

// -------------------- Non-blocking WiFi scan cache --------------------
namespace NetScanCache
{
  static bool scanRunning = false;
  static uint32_t cacheTs = 0;
  static String cacheJson;
  static const uint32_t CACHE_MS = 10000;

  static bool cacheValid()
  {
    if (cacheTs == 0) return false;
    return (millis() - cacheTs) < CACHE_MS && cacheJson.length() > 0;
  }

  static void startAsyncScanIfNeeded(bool force)
  {
    if (!force && cacheValid()) return;

    int sc = WiFi.scanComplete();
    if (sc == WIFI_SCAN_RUNNING) {
      scanRunning = true;
      return;
    }

    // If results available (>=0), let collectIfFinished() harvest them.
    // Otherwise start a new async scan.
    if (sc < 0) {
      int rc = WiFi.scanNetworks(true /* async */, true /* show hidden */);
      scanRunning = (rc == WIFI_SCAN_RUNNING);
    } else {
      scanRunning = false;
    }
  }

  static void collectIfFinished()
  {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      scanRunning = true;
      return;
    }
    if (n < 0) {
      scanRunning = false;
      return;
    }

    scanRunning = false;

    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();

    for (int i = 0; i < n; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"]  = WiFi.SSID(i);
      o["rssi"]  = WiFi.RSSI(i);
      o["enc"]   = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      o["bssid"] = WiFi.BSSIDstr(i);
    }

    WiFi.scanDelete();

    cacheJson = "";
    serializeJson(doc, cacheJson);
    cacheTs = millis();
  }

  static const String& json()
  {
    return cacheJson;
  }
} // namespace NetScanCache

// -------------------- Restart scheduling (no delay in handlers) --------------------
static void bb_restart_cb(void* arg)
{
  (void)arg;
  ESP.restart();
}

static void scheduleRestart(uint32_t delayMs)
{
  esp_timer_handle_t t = nullptr;
  esp_timer_create_args_t args = {};
  args.callback = &bb_restart_cb;
  args.arg = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "bb_restart";

  if (esp_timer_create(&args, &t) == ESP_OK && t) {
    esp_timer_start_once(t, (uint64_t)delayMs * 1000ULL);
  } else {
    ESP.restart();
  }
}

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
  // Never run synchronous WiFi scans inside AsyncTCP handlers.
  // Trigger async scan and return cached results immediately.
  NetScanCache::startAsyncScanIfNeeded(false);
  NetScanCache::collectIfFinished();

  if (NetScanCache::cacheValid()) {
    req->send(200, "application/json", NetScanCache::json());
    return;
  }

  // No cache yet -> respond with empty list (HTML remains unchanged).
  req->send(200, "application/json", "{\"networks\":[]}");
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

  // FIX: ssid1 must go to wifiSsid1 (was wrongly written to wifiSsid0)
  settings.set.wifiSsid1(getP("ssid1"));
  settings.set.wifiPass1(getP("password1"));

  settings.set.staticIP(getP("ip"));
  settings.set.staticSN(getP("subnet"));
  settings.set.staticGW(getP("gateway"));
  settings.set.staticDNS(getP("dns"));

  settings.set.webUIuser(getP("webUser"));
  settings.set.webUIPass(getP("webPass"));

  settings.save();

  req->send(200, "application/json", "{\"success\":true}");

  // Do not block inside async request handlers
  scheduleRestart(600);
}

void WebServerHandler::handlePrinterDiscovery(AsyncWebServerRequest* req) {
  if (req->hasParam("rescan")) {
    printerDiscovery.forceRescan(0);
  }

  JsonDocument doc;
  JsonArray arr = doc["printers"].to<JsonArray>();

  const int n = printerDiscovery.knownCount();
  const BBLPrinter* printers = printerDiscovery.knownPrinters();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["usn"] = printers[i].usn;
    o["ip"] = printers[i].ip.toString();
  }

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

void WebServerHandler::handleSubmitPrinterConfig(AsyncWebServerRequest* req) {
  auto getP = [&](const char* name) -> String {
    if (!req->hasParam(name, true)) return "";
    return req->getParam(name, true)->value();
  };

  const uint16_t oldSeg = settings.get.LEDSegments();
  const uint16_t oldPer = settings.get.LEDperSeg();

  settings.set.printerIP(getP("printerip"));
  settings.set.printerUSN(getP("printerusn"));
  settings.set.printerAC(getP("printerac"));

  if (req->hasParam("ledsegments", true)) {
    long v = getP("ledsegments").toInt();
    if (v < 2) v = 2;
    if (v > 3) v = 3;
    settings.set.LEDSegments((uint16_t)v);
  }

  if (req->hasParam("ledperseg", true)) {
    long v = getP("ledperseg").toInt();
    if (v < 1) v = 1;
    if (v > 64) v = 64;
    settings.set.LEDperSeg((uint16_t)v);
  }

  if (req->hasParam("ledmaxcurrent", true)) {
    long v = getP("ledmaxcurrent").toInt();
    if (v < 100) v = 100;
    if (v > 5000) v = 5000;
    settings.set.LEDMaxCurrentmA((uint16_t)v);
  }

  if (req->hasParam("ledreverse", true)) {
    const String v = getP("ledreverse");
    const bool enabled = (v == "1" || v == "true" || v == "on");
    settings.set.LEDReverseOrder(enabled);
  }

  settings.save();
  ledsCtrl.applySettingsFrom(settings);

  bambu.reloadFromSettings();
  if (WiFi.status() == WL_CONNECTED) bambu.connect();

  req->send(200, "application/json", "{\"success\":true}");

  if (settings.get.LEDSegments() != oldSeg || settings.get.LEDperSeg() != oldPer) {
    scheduleRestart(600);
  }
}

void WebServerHandler::handleLedTestCmd(AsyncWebServerRequest* req) {
  auto getP = [&](const char* name) -> String {
    if (!req->hasParam(name, true)) return "";
    return req->getParam(name, true)->value();
  };

  const String action = getP("action");
  const String value = getP("value");

  if (action == "mode") {
    const bool enable = (value == "on" || value == "1" || value == "true");
    ledsCtrl.setTestMode(enable);
    req->send(200, "application/json", "{\"success\":true}");
    return;
  }

  if (!ledsCtrl.testMode()) {
    req->send(200, "application/json", "{\"success\":false,\"reason\":\"testmode\"}");
    return;
  }

  if (action == "state") {
    ledsCtrl.testSetState(value);
  } else if (action == "wifi") {
    ledsCtrl.testSetWifi(value != "0");
  } else if (action == "mqtt") {
    ledsCtrl.testSetMqtt(value != "0");
  } else if (action == "print") {
    long v = value.toInt();
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    ledsCtrl.testSetPrintProgress((uint8_t)v);
  } else if (action == "download") {
    long v = value.toInt();
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    ledsCtrl.testSetDownloadProgress((uint8_t)v);
  } else {
    req->send(400, "application/json", "{\"success\":false}");
    return;
  }

  req->send(200, "application/json", "{\"success\":true}");
}

void WebServerHandler::begin() {
  auto captivePortalResponse = [&](AsyncWebServerRequest* req) {
    if (wifiManager.isApMode()) {
      sendGz(req, WiFiSetup_html_gz, WiFiSetup_html_gz_len, WiFiSetup_html_gz_mime);
      return;
    }
    req->send(404, "text/plain", "Not found");
  };

  // Root
  server.on("/", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (wifiManager.isApMode()) {
      req->redirect("/wifisetup");
      return;
    }
    if (!isAuthorized(req)) return req->requestAuthentication();
    sendGz(req, Status_html_gz, Status_html_gz_len, Status_html_gz_mime);
  });

  // WiFi setup should always be reachable in AP mode without login
  server.on("/wifisetup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    // Start scan aggressively when entering setup page
    NetScanCache::startAsyncScanIfNeeded(true);
    sendGz(req, WiFiSetup_html_gz, WiFiSetup_html_gz_len, WiFiSetup_html_gz_mime);
  });

  // Captive portal detection endpoints (Android/iOS/Windows)
  server.on("/generate_204", HTTP_GET, captivePortalResponse);
  server.on("/gen_204", HTTP_GET, captivePortalResponse);
  server.on("/hotspot-detect.html", HTTP_GET, captivePortalResponse);
  server.on("/library/test/success.html", HTTP_GET, captivePortalResponse);
  server.on("/ncsi.txt", HTTP_GET, captivePortalResponse);
  server.on("/connecttest.txt", HTTP_GET, captivePortalResponse);
  server.on("/fwlink", HTTP_GET, captivePortalResponse);

  server.on("/printersetup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGz(req, PrinterSetup_html_gz, PrinterSetup_html_gz_len, PrinterSetup_html_gz_mime);
  });

  server.on("/maintenance", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGz(req, Maintenance_html_gz, Maintenance_html_gz_len, Maintenance_html_gz_mime);
  });

  server.on("/ledtest", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGz(req, LedTest_html_gz, LedTest_html_gz_len, LedTest_html_gz_mime);
  });

  server.on("/style.css", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, Style_css_gz, Style_css_gz_len, Style_css_gz_mime);
  });

  server.on("/logo.svg", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, logo_svg_gz, logo_svg_gz_len, logo_svg_gz_mime);
  });

  server.on("/favicon.ico", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, logo_ico_gz, logo_ico_gz_len, logo_ico_gz_mime);
  });

  server.on("/backgroundCanvas.js", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, backgroundCanvas_js_gz, backgroundCanvas_js_gz_len, backgroundCanvas_js_gz_mime);
  });

  server.on("/footer.js", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, footer_js_gz, footer_js_gz_len, footer_js_gz_mime);
  });

  server.on("/netlist", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleNetlist(req);
  });

  server.on("/bblprinterdiscovery", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handlePrinterDiscovery(req);
  });

  server.on("/submitConfig", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleSubmitConfig(req);
  });

  server.on("/submitPrinterConfig", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleSubmitPrinterConfig(req);
  });

  server.on("/ledtestcmd", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleLedTestCmd(req);
  });

  server.on("/config/backup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    const bool pretty = req->hasParam("pretty");
    const String out = settings.backup(pretty);
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
    r->addHeader("Content-Disposition", "attachment; filename=bambubeacon-backup.json");
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/config/restore", HTTP_POST,
    [&](AsyncWebServerRequest* req) {
      if (!wifiManager.isApMode()) {
        if (!isAuthorized(req)) {
          if (req->_tempObject) {
            delete (String*)req->_tempObject;
            req->_tempObject = nullptr;
          }
          return req->requestAuthentication();
        }
      }

      String* body = (String*)req->_tempObject;
      const bool ok = body && settings.restore(*body, true, true);
      if (body) {
        delete body;
        req->_tempObject = nullptr;
      }

      if (ok) {
        req->send(200, "application/json", "{\"success\":true}");
        scheduleRestart(600);
      } else {
        req->send(400, "application/json", "{\"success\":false}");
      }
    },
    nullptr,
    [&](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      if (!wifiManager.isApMode() && !isAuthorized(req)) return;
      String* body = (String*)req->_tempObject;
      if (!body) {
        body = new String();
        body->reserve(total);
        req->_tempObject = body;
      }
      body->concat((const char*)data, len);
    }
  );

  server.on("/update", HTTP_POST,
    [&](AsyncWebServerRequest* req) {
      if (!wifiManager.isApMode()) {
        if (!isAuthorized(req)) return req->requestAuthentication();
      }
      const bool ok = !Update.hasError();
      req->send(ok ? 200 : 500, "application/json", ok ? "{\"success\":true}" : "{\"success\":false}");
      if (ok) scheduleRestart(2500);
    },
    [&](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      (void)filename;
      if (!wifiManager.isApMode() && !isAuthorized(req)) return;
      if (index == 0) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(webSerial);
        }
      }
      if (Update.write(data, len) != len) {
        Update.printError(webSerial);
      }
      if (final) {
        if (!Update.end(true)) {
          Update.printError(webSerial);
        }
      }
    }
  );

  server.on("/ota/status", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    const String out = ota.statusJson();
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/ota/check", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    ota.requestCheck();
    const String out = ota.statusJson();
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/ota/update", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    ota.startUpdate();
    const String out = ota.statusJson();
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/netconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["deviceName"] = settings.get.deviceName();
    doc["ssid0"] = settings.get.wifiSsid0();
    doc["pass0"] = settings.get.wifiPass0();
    doc["bssid0"] = settings.get.wifiBssid0();
    doc["ssid1"] = settings.get.wifiSsid1();
    doc["pass1"] = settings.get.wifiPass1();
    doc["ip"] = settings.get.staticIP();
    doc["subnet"] = settings.get.staticSN();
    doc["gateway"] = settings.get.staticGW();
    doc["dns"] = settings.get.staticDNS();
    doc["webUser"] = settings.get.webUIuser();
    // FIX: return the password, not the user name
    doc["webPass"] = settings.get.webUIPass();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/printerconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["printerIP"] = settings.get.printerIP();
    doc["printerUSN"] = settings.get.printerUSN();
    doc["printerAC"] = settings.get.printerAC();
    doc["ledSegments"] = settings.get.LEDSegments();
    doc["ledPerSeg"] = settings.get.LEDperSeg();
    doc["ledMaxCurrentmA"] = settings.get.LEDMaxCurrentmA();
    doc["ledReverseOrder"] = settings.get.LEDReverseOrder();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/ledconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["ledBrightness"] = settings.get.LEDBrightness();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/setLedBrightness", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    if (!req->hasParam("brightness", true)) {
      req->send(400, "application/json", "{\"success\":false}");
      return;
    }

    const String v = req->getParam("brightness", true)->value();
    long b = v.toInt();
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    settings.set.LEDBrightness((uint16_t)b);
    settings.save();
    ledsCtrl.setBrightness((uint8_t)b);

    req->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/info.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!isAuthorized(req)) return req->requestAuthentication();

    JsonDocument doc;
    doc["deviceName"] = settings.get.deviceName();
    doc["mode"] = wifiManager.isApMode() ? "AP" : "STA";
    doc["ip"] = wifiManager.isApMode() ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    doc["version"] = STRVERSION;

    String out;
    serializeJson(doc, out);
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
