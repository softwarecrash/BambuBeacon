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
#include "GitHubOtaUpdater.h"
#include "WireGuardVpnManager.h"
#include "VpnSecretStore.h"
extern "C" {
#include "wireguard-platform.h"
}

LedController ledsCtrl;
Settings settings;
WiFiManager wifiManager;
AsyncWebServer server(80);
WebServerHandler web(server);
BBLPrinterDiscovery printerDiscovery;
BambuMqttClient bambu;
GitHubOtaUpdater ota("softwarecrash", "BambuBeacon", STRVERSION, BUILD_VARIANT);
WireGuardVpnManager wireGuardVpn;

static IPAddress parseIpOrDefault(const char* value, const IPAddress& fallback) {
  IPAddress ip;
  if (value && ip.fromString(value)) {
    return ip;
  }
  return fallback;
}

static VpnConfig vpnConfigFromSettings() {
  VpnConfig cfg;
  cfg.enabled = settings.get.vpnEnabled();
  cfg.localIp = parseIpOrDefault(settings.get.vpnLocalIp(), IPAddress(0, 0, 0, 0));
  cfg.localMask = parseIpOrDefault(settings.get.vpnLocalMask(), IPAddress(255, 255, 255, 0));
  cfg.localPort = settings.get.vpnLocalPort();
  cfg.localGateway = parseIpOrDefault(settings.get.vpnLocalGateway(), IPAddress(0, 0, 0, 0));
  if (!VpnSecretStore::loadPrivateKey(&cfg.privateKey)) {
    cfg.privateKey = "";
  }
  cfg.endpointHost = settings.get.vpnEndpointHost() ? settings.get.vpnEndpointHost() : "";
  cfg.endpointPublicKey = settings.get.vpnEndpointPubKey() ? settings.get.vpnEndpointPubKey() : "";
  cfg.endpointPort = settings.get.vpnEndpointPort();
  cfg.allowedIp = parseIpOrDefault(settings.get.vpnAllowedIp(), IPAddress(0, 0, 0, 0));
  cfg.allowedMask = parseIpOrDefault(settings.get.vpnAllowedMask(), IPAddress(0, 0, 0, 0));
  cfg.makeDefault = false;
  if (!VpnSecretStore::loadPresharedKey(&cfg.presharedKey)) {
    cfg.presharedKey = "";
  }
  return cfg;
}

void setup() {
#ifdef WSL_CUSTOM_PAGE
  webSerial.setCustomHtmlPage(webserialHtml(), webserialHtmlLen(), "gzip");
#endif
  webSerial.begin(&server, 115200, 200);

  // Initialize WireGuard platform crypto once in single-thread setup phase.
  // Avoids late concurrent first-init paths when VPN starts.
  wireguard_platform_init();

  settings.begin();
  (void)VpnSecretStore::privateKeyMeta();
  (void)VpnSecretStore::presharedKeyMeta();
  webSerial.setAuthentication(settings.get.webUIuser(), settings.get.webUIPass());
  ota.begin();
  ota.setUpdateActivityCallback([](bool active) {
    if (active) {
      webSerial.println("[MQTT] Paused for OTA");
      bambu.disconnect();
    } else if (WiFi.status() == WL_CONNECTED) {
      bambu.connect();
    }
  });
  ledsCtrl.begin(settings);
  wifiManager.begin();
  web.begin();
  wireGuardVpn.begin(vpnConfigFromSettings());

  bambu.onReport([](uint32_t nowMs) {
    ledsCtrl.ingestBambuReport(nowMs);
  });
 bambu.begin(settings);

printerDiscovery.begin();
printerDiscovery.setInterval(60000UL);
printerDiscovery.setListenWindow(4000UL);  // 4s listen window
printerDiscovery.forceRescan(2000UL);      // first scan shortly after boot



  webSerial.println("[BOOT] BambuBeacon started");
}

void loop() {
  ledsCtrl.loop();
  if (ledsCtrl.bootTestActive()) {
    return;
  }
  wifiManager.loop();
  wireGuardVpn.update();
  printerDiscovery.update();
  if (bambu.isConnected() || !printerDiscovery.isBusy()) {
    bambu.loopTick();
  }
  const uint32_t nowMs = millis();
  static bool otaAutoInit = false;
  static bool otaAutoDisabled = false;
  static bool otaCheckInFlight = false;
  static uint32_t otaNextCheckMs = 0;
  if (!otaAutoInit) {
    otaAutoInit = true;
    otaNextCheckMs = nowMs + 60000UL;
  }
  if (!otaAutoDisabled) {
    if (!otaCheckInFlight && !ota.isBusy()) {
      if ((int32_t)(nowMs - otaNextCheckMs) >= 0) {
        if (WiFi.status() == WL_CONNECTED) {
          if (ota.requestCheck()) {
            otaCheckInFlight = true;
          }
        }
      }
    }
    if (otaCheckInFlight) {
      bool netFail = false;
      if (ota.takeLastCheckResult(&netFail)) {
        otaCheckInFlight = false;
        if (netFail) {
          otaAutoDisabled = true;
        } else {
          otaNextCheckMs = nowMs + 12UL * 60UL * 60UL * 1000UL;
        }
      }
    }
  }
  ledsCtrl.setMqttConnected(bambu.isConnected(), nowMs);
  ledsCtrl.setHmsSeverity((uint8_t)bambu.topSeverity());
  ledsCtrl.setWifiConnected(WiFi.status() == WL_CONNECTED);

  const String gstate = bambu.gcodeState();
  const bool finished = (gstate == "FINISH" || gstate == "FINISHED" || gstate == "DONE");
  const bool paused = (gstate == "PAUSE" || gstate == "PAUSED");
  const bool printing = (gstate == "RUNNING" || gstate == "PRINTING" || paused || gstate == "PREPARE");

  uint8_t dl = bambu.downloadProgress();
  ledsCtrl.setDownloadProgress((dl <= 100 && dl < 100) ? dl : 255);

  uint8_t pp = bambu.printProgress();
  ledsCtrl.setPrintProgress((printing && pp <= 100 && pp < 100) ? pp : 255);
  ledsCtrl.setUpdateAvailable(ota.isUpdateAvailable());
  if (!ledsCtrl.otaManualActive()) {
    ledsCtrl.setOtaProgress(ota.isDownloading() ? ota.progressPercent() : 255);
  }

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
  static uint32_t finishSinceMs = 0;
  if (finished && finishSinceMs == 0) {
    finishSinceMs = nowMs;
  } else if (!finished) {
    finishSinceMs = 0;
  }
  const uint32_t FINISH_MIN_MS = 5UL * 60UL * 1000UL;
  const bool finishMinActive = (finishSinceMs != 0) && (uint32_t)(nowMs - finishSinceMs) < FINISH_MIN_MS;
  const bool bedHot = bambu.bedValid() && (bambu.bedTemp() > 45.0f);
  const bool showFinish = finished && (finishMinActive || bedHot);
  ledsCtrl.setFinished(showFinish);
  ledsCtrl.loop();
}
