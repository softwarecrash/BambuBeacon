#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <functional>
#include <WebSerial.h>
#include <WiFiClientSecure.h>
#include <mqtt_client.h>
#include <time.h>

#include "SettingsPrefs.h"  // provides Settings + settings.get.printerIP/printerUSN/printerAC

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

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

  struct StreamParser {
    void reset();
    bool feed(const uint8_t* data, size_t len);
    bool finish(ParsedReport& out);

  private:
    enum class KeyId : uint8_t {
      Root,
      Print,
      GcodeState,
      McPercent,
      Percent,
      DownloadProgress,
      DownloadPercent,
      DlPercent,
      DlProgress,
      PreparePer,
      GcodeFilePreparePercent,
      BedTemper,
      BedTemperature,
      BedTargetTemper,
      BedTargetTemperature,
      NozzleTemper,
      NozzleTargetTemper,
      Device,
      Extruder,
      Info,
      Hnow,
      Htar,
      Temp,
      Hms,
      Attr,
      Code,
      Data,
      Unknown
    };

    enum class Mode : uint8_t {
      Default,
      InStringKey,
      InStringVal,
      InNumber,
      InLiteral
    };

    struct Ctx {
      bool isArray = false;
      bool expectingKey = false;
      bool expectingValue = false;
      KeyId key = KeyId::Unknown;
      bool isHmsArray = false;
      bool isExtruderInfoArray = false;
      bool isHmsItem = false;
      int index = -1;
    };

    static KeyId keyIdFromString(const char* s, size_t len);

    void pushObject();
    void pushArray();
    void popContext();
    void valueCompleted();
    KeyId parentKey() const;
    KeyId grandParentKey() const;
    int currentExtruderInfoIndex() const;
    bool inExtruderInfoArray() const;
    bool inHmsItem() const;

    void handleValueString(const char* s, size_t len);
    void handleValueNumber(const char* s, size_t len);
    void handleValueLiteral(const char* s, size_t len);
    bool parseInt(const char* s, size_t len, int& out) const;
    bool parseFloat(const char* s, size_t len, float& out) const;
    bool isNumberChar(char c) const;
    void addHmsIfReady();

    Mode _mode = Mode::Default;
    bool _escape = false;
    char _strBuf[48] = {0};
    size_t _strLen = 0;
    char _numBuf[32] = {0};
    size_t _numLen = 0;
    char _litBuf[8] = {0};
    size_t _litLen = 0;
    KeyId _currentKey = KeyId::Unknown;
    bool _error = false;

    Ctx _stack[10];
    int _depth = 0;

    ParsedReport _report;
    bool _bedOk = false;
    bool _bedTargetOk = false;
    float _bedTemp = 0.0f;
    float _bedTarget = 0.0f;
    bool _nozOk = false;
    bool _nozTargetOk = false;
    float _nozTemp = 0.0f;
    float _nozTarget = 0.0f;
    bool _nozzleHeatingCandidate = false;
    bool _hmsArraySeen = false;
    uint32_t _hmsAttr = 0;
    uint32_t _hmsCode = 0;
    bool _hmsAttrSet = false;
    bool _hmsCodeSet = false;
  };

  void buildFromSettings();
  bool configLooksValid() const;

  void subscribeReportOnce();
  static esp_err_t mqttEventHandler(esp_mqtt_event_handle_t event);
  esp_err_t handleEvent(esp_mqtt_event_handle_t event);
  bool topicMatches(const char* topic, int topicLen) const;
  bool handleMqttData(esp_mqtt_event_handle_t event);
  bool initClientFromSettings();
  void resetClient();
  bool timeIsValid() const;
  void ensureTimeSync();
  void fetchCertSync(const char* reason);
  void applyParsedReport(const ParsedReport& report);
  void logStatusIfNeeded(uint32_t nowMs);
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
  uint8_t _parseOk = 0;
  uint8_t _parseFail = 0;
  uint32_t _lastParseLogMs = 0;

  ReportCallback _reportCb;

  StreamParser _streamParser;
  size_t _rxExpected = 0;
  size_t _rxReceived = 0;
  bool _rxTopicMatch = false;

  bool _certFetchInProgress = false;
  bool _certPendingSave = false;
  bool _pendingClientReset = false;
  bool _clearStoredCert = false;
  bool _timeSyncStarted = false;
  bool _timeSyncOk = false;
  uint32_t _lastCertFetchMs = 0;
  char* _fetchedCert = nullptr;
  size_t _fetchedCertLen = 0;

  SemaphoreHandle_t _pendingMutex = nullptr;
  ParsedReport _pendingReport;
  bool _pendingReady = false;
};
