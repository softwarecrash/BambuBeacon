#include <Arduino.h>
#include "main.h"
#include "LedController.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include "bblPrinterDiscovery.h"
#include "SettingsPrefs.h"
#include "WiFiManager.h"
#include "BambuMqttClient.h"
#include "WebServerHandler.h"
#include "WebSerial.h"

LedController ledsCtrl;
Settings settings;
WiFiManager wifiManager;
AsyncWebServer server(80);
WebServerHandler web(server);
BBLPrinterDiscovery printerDiscovery;
BambuMqttClient bambu;

void setup() {
#ifdef WSL_CUSTOM_PAGE
  webSerial.setCustomHtmlPage(webserialHtml(), webserialHtmlLen(), "gzip");
#endif
  webSerial.begin(&server, 115200, 200);

  settings.begin();
  webSerial.setAuthentication(settings.get.webUIuser(), settings.get.webUIPass());
  ledsCtrl.begin(settings);
  wifiManager.begin();
  web.begin();

  bambu.onReport([](const JsonDocument& doc) {
    ledsCtrl.ingestBambuReport(doc.as<JsonObjectConst>(), millis());
  });
 bambu.begin(settings);

printerDiscovery.begin();
printerDiscovery.setInterval(60000UL);     // Normalbetrieb: 60s
printerDiscovery.setListenWindow(4000UL);  // 4s listen window
printerDiscovery.forceRescan(2000UL);      // first scan shortly after boot



  webSerial.println("[BOOT] BambuBeacon started");
}

void loop() {
  wifiManager.loop();
  printerDiscovery.update();
  if (bambu.isConnected() || !printerDiscovery.isBusy()) {
    bambu.loopTick();
  }
  const uint32_t nowMs = millis();
  ledsCtrl.setMqttConnected(bambu.isConnected(), nowMs);
  ledsCtrl.setHmsSeverity((uint8_t)bambu.topSeverity());
  ledsCtrl.setWifiConnected(WiFi.status() == WL_CONNECTED);

  const String& gstate = bambu.gcodeState();
  const bool finished = (gstate == "FINISH" || gstate == "FINISHED" || gstate == "DONE");
  const bool paused = (gstate == "PAUSE" || gstate == "PAUSED");
  const bool printing = (gstate == "RUNNING" || gstate == "PRINTING" || paused || gstate == "PREPARE");

  uint8_t dl = bambu.downloadProgress();
  ledsCtrl.setDownloadProgress((dl <= 100 && dl < 100) ? dl : 255);

  uint8_t pp = bambu.printProgress();
  ledsCtrl.setPrintProgress((printing && pp <= 100 && pp < 100) ? pp : 255);

  bool heating = false;
  bool cooling = false;
  if (bambu.bedValid()) {
    heating = !finished && (bambu.bedTarget() > (bambu.bedTemp() + 2.0f));
    cooling = finished && (bambu.bedTemp() > 45.0f);
  }
  if (bambu.nozzleHeating()) {
    heating = true;
  } else if (bambu.nozzleValid()) {
    heating = heating || (!finished && (bambu.nozzleTarget() > (bambu.nozzleTemp() + 2.0f)));
  }
  ledsCtrl.setThermalState(heating, cooling);
  ledsCtrl.setPaused(paused);
  ledsCtrl.setFinished(finished);
  ledsCtrl.loop();
}
