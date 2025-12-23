#include <Arduino.h>
#include "main.h"
#include "LedController.h"
#include <ESPAsyncWebServer.h>
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
  Serial.begin(115200);
  delay(50);

  webSerial.begin(&server, 115200, 200);

  settings.begin();
  ledsCtrl.begin(settings);
  wifiManager.begin();
  web.begin();

  bambu.onReport([](const JsonDocument& doc) {
    // optional

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
  bambu.loopTick();
  ledsCtrl.loop();
  printerDiscovery.update();
}
