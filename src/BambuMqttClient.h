#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <functional>
#include <WebSerial.h>
#include <WiFiClientSecure.h>
#include <mqtt_client.h>
#include <time.h>

#include "SettingsPrefs.h"  // provides Settings + settings.get.printerIP/printerUSN/printerAC

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

class BambuMqttClient {
public:
  enum class Severity : uint8_t {
    None = 0,
    Info,
    Warning, // Common
    Error,   // Serious
    Fatal
  };

  struct HmsEvent {
    uint64_t full = 0;
    uint32_t attr = 0;
    uint32_t code = 0;

    char codeStr[24] = {0}; // "HMS_XXXX_XXXX_XXXX_XXXX"
    Severity severity = Severity::None;

    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs = 0;
    uint32_t count = 0;
    bool active = false;
  };

  using ReportCallback = std::function<void(uint32_t nowMs)>;

  BambuMqttClient();
  ~BambuMqttClient();

  // Uses settings.get.printerIP(), settings.get.printerUSN(), settings.get.printerAC()
  // Fixed: host=printerIP, user=bblp, port=8883, pass=printerAC
  // Safe if settings are incomplete -> will not connect and will not crash.
  bool begin(Settings &settings);

  void loopTick();
  void connect();
  void disconnect();

  bool isConnected();

  bool publishRequest(const JsonDocument& doc, bool retain = false);
  void onReport(ReportCallback cb);

  // HMS / status
  Severity topSeverity() const;
  bool hasProblem() const; // >= Warning
  uint16_t countActive(Severity sev) const;
  uint16_t countActiveTotal() const;
  size_t getActiveEvents(HmsEvent* out, size_t maxOut) const;

  String gcodeState() const;
  uint8_t printProgress() const;
  uint8_t downloadProgress() const;
  float bedTemp() const;
  float bedTarget() const;
  bool bedValid() const;
  float nozzleTemp() const;
  float nozzleTarget() const;
  bool nozzleValid() const;
  bool nozzleHeating() const;

  const String& topicReport() const;
  const String& topicRequest() const;

  // Call after user updated printer settings in UI (IP/USN/AC)
  void reloadFromSettings();

private:
  struct ParsedHmsEntry {
    uint32_t attr = 0;
    uint32_t code = 0;
  };

  struct ParsedReport {
    bool hasGcodeState = false;
    char gcodeState[32] = {0};
    bool hasPrintProgress = false;
    uint8_t printProgress = 255;
    bool hasDownloadProgress = false;
    uint8_t downloadProgress = 255;
    bool hasBed = false;
    float bedTemp = 0.0f;
    float bedTarget = 0.0f;
    bool hasNozzleTemp = false;
    float nozzleTemp = 0.0f;
    bool hasNozzleTarget = false;
    float nozzleTarget = 0.0f;
    bool nozzleHeating = false;
    bool hmsPresent = false;
    uint8_t hmsCount = 0;
    ParsedHmsEntry hms[20];
    uint32_t nowMs = 0;
  };

  struct RawMsg {
    uint8_t* payload = nullptr;
    size_t length = 0;
  };

  void buildFromSettings();
  bool configLooksValid() const;

  void subscribeReportOnce();
  void handleReportJson(const uint8_t* payload, size_t length);
  static esp_err_t mqttEventHandler(esp_mqtt_event_handle_t event);
  esp_err_t handleEvent(esp_mqtt_event_handle_t event);
  bool topicMatches(const char* topic, int topicLen) const;
  bool handleMqttData(esp_mqtt_event_handle_t event);
  bool initClientFromSettings();
  void resetClient();
  bool timeIsValid() const;
#if defined(ARDUINO_ARCH_ESP32)
  void ensureTimeSync();
  void fetchCertSync(const char* reason);
#endif
  bool parseReportJson(const uint8_t* payload, size_t length, ParsedReport& out);
  void applyParsedReport(const ParsedReport& report);
  void logStatusIfNeeded(uint32_t nowMs);

  JsonArray findHmsArray(JsonDocument& doc);
  bool isIgnored(const char* codeStr) const;

  static Severity severityFromCode(uint32_t code);
  static void formatHmsCodeStr(uint64_t full, char out[24]);

  void upsertEvent(uint32_t attr, uint32_t code, uint32_t nowMs);
  void expireEvents(uint32_t nowMs);
  Severity computeTopSeverity() const;

private:
  Settings *_settings = nullptr;

  esp_mqtt_client_handle_t _client = nullptr;
  bool _clientStarted = false;
  bool _connected = false;
  bool _subscribed = false;

  // Derived config (always from settings)
  String _printerIP;
  String _serial;
  String _accessCode;
  String _clientId;

  // Fixed
  static const uint16_t kPort = 8883;
  static const char*    kUser;
  static const size_t   kMqttBufferSize = 32768;
  static const size_t   kMqttHeapSafety = 12 * 1024;

  String _serverUri;
  String _topicReport;
  String _topicRequest;

  // HMS
  String _ignoreNorm;
  uint32_t _hmsTtlMs = 20000;
  uint8_t _eventsCap = 20;

  String _gcodeState;
  uint8_t _printProgress = 255;    // 0-100, 255 = unknown
  uint8_t _downloadProgress = 255; // 0-100, 255 = unknown
  float _bedTemp = 0.0f;
  float _bedTarget = 0.0f;
  bool _bedValid = false;
  float _nozzleTemp = 0.0f;
  float _nozzleTarget = 0.0f;
  bool _nozzleValid = false;
  bool _nozzleHeating = false;

  HmsEvent* _events = nullptr;

  // NEW: safe guard when settings are incomplete or begin() not successful
  bool _ready = false;

  uint32_t _lastStatusLogMs = 0;
  String _lastStatusState;
  uint8_t _lastStatusPrint = 255;
  uint8_t _lastStatusDownload = 255;
  Severity _lastStatusSeverity = Severity::None;
  uint16_t _lastStatusHmsCount = 0;
  uint32_t _lastMsgMs = 0;
  uint32_t _lastMsgLen = 0;
  uint32_t _lastMqttDebugMs = 0;
  uint32_t _lastReportLogMs = 0;

  ReportCallback _reportCb;

  uint8_t* _rxBuf = nullptr;
  size_t _rxLen = 0;
  size_t _rxExpected = 0;
  bool _rxTopicMatch = false;
  size_t _maxPayloadSeen = 0;
  size_t _maxPayloadDropped = 0;
  uint32_t _droppedOversize = 0;
  uint32_t _droppedAlloc = 0;

#if defined(ARDUINO_ARCH_ESP32)
  bool _certFetchInProgress = false;
  bool _certPendingSave = false;
  bool _pendingClientReset = false;
  bool _clearStoredCert = false;
  bool _timeSyncStarted = false;
  bool _timeSyncOk = false;
  uint32_t _lastCertFetchMs = 0;
  char* _fetchedCert = nullptr;
  size_t _fetchedCertLen = 0;
#endif

#if defined(ARDUINO_ARCH_ESP32)
  static void parserTaskThunk(void* arg);
  void parserTask();

  QueueHandle_t _parseQueue = nullptr;
  SemaphoreHandle_t _pendingMutex = nullptr;
  TaskHandle_t _parserTask = nullptr;
  ParsedReport _pendingReport;
  bool _pendingReady = false;
  uint32_t _droppedMsgs = 0;
#endif
};
