#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

class WebServerHandler {
public:
  explicit WebServerHandler(AsyncWebServer& s);
  void begin();

private:
  AsyncWebServer& server;

  bool isAuthorized(AsyncWebServerRequest* req);
  void sendGz(AsyncWebServerRequest* req, const uint8_t* data, size_t len, const char* mime);
  void sendGzChunked(AsyncWebServerRequest* req, const uint8_t* data, size_t len, const char* mime);

  void handleNetlist(AsyncWebServerRequest* req);
  void handleSubmitConfig(AsyncWebServerRequest* req);
  void handlePrinterDiscovery(AsyncWebServerRequest* req);
  void handleSubmitPrinterConfig(AsyncWebServerRequest* req);
  void handleLedTestCmd(AsyncWebServerRequest* req);
  void handleGetVpnApi(AsyncWebServerRequest* req);
  void handleSetVpnApi(AsyncWebServerRequest* req, const String& body);
};

const uint8_t* webserialHtml();
size_t webserialHtmlLen();
