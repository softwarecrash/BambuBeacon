#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "SettingsPrefs.h"
#include "WiFiManager.h"
#include "WebServerHandler.h"
#include "webSerial.h"

Settings settings;
WiFiManager wifiManager;

AsyncWebServer server(80);
WebServerHandler web(server);

void setup() {
  Serial.begin(115200);
  delay(50);

  settings.begin();

  wifiManager.begin();
  web.begin();

  webSerial.println("[BOOT] BambuBeacon started");
}

void loop() {
  wifiManager.loop();
}
