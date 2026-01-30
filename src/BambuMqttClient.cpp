#include "BambuMqttClient.h"
#include <mbedtls/pem.h>
#include <mbedtls/x509_crt.h>
#include <new>

namespace {
constexpr uint32_t kSocketTimeoutMs = 5000;
const char* severityToStr(BambuMqttClient::Severity s) {
  switch (s) {
    case BambuMqttClient::Severity::Fatal: return "Fatal";
    case BambuMqttClient::Severity::Error: return "Error";
    case BambuMqttClient::Severity::Warning: return "Warning";
    case BambuMqttClient::Severity::Info: return "Info";
    default: return "None";
  }
}
}

const char* BambuMqttClient::kUser = "bblp";

#if defined(ARDUINO_ARCH_ESP32)
void BambuMqttClient::parserTaskThunk(void* arg) {
  if (arg) {
    static_cast<BambuMqttClient*>(arg)->parserTask();
  }
  vTaskDelete(nullptr);
}

#endif

BambuMqttClient::BambuMqttClient() {}
BambuMqttClient::~BambuMqttClient() {
#if defined(ARDUINO_ARCH_ESP32)
  if (_parserTask) {
    vTaskDelete(_parserTask);
    _parserTask = nullptr;
  }
  if (_parseQueue) {
    RawMsg msg;
    while (xQueueReceive(_parseQueue, &msg, 0) == pdTRUE) {
      delete[] msg.payload;
    }
    vQueueDelete(_parseQueue);
    _parseQueue = nullptr;
  }
  if (_pendingMutex) {
    vSemaphoreDelete(_pendingMutex);
    _pendingMutex = nullptr;
  }
  if (_fetchedCert) {
    delete[] _fetchedCert;
    _fetchedCert = nullptr;
  }
#endif
  if (_client) {
    esp_mqtt_client_stop(_client);
    esp_mqtt_client_destroy(_client);
    _client = nullptr;
  }
  if (_rxBuf) {
    delete[] _rxBuf;
    _rxBuf = nullptr;
  }
  if (_events) {
    delete[] _events;
    _events = nullptr;
  }
}

bool BambuMqttClient::begin(Settings &settings) {
  _settings = &settings;
  _ready = false;

  buildFromSettings();

  if (!configLooksValid()) {
    webSerial.println("[MQTT] Missing settings (printerIP/printerUSN/printerAC). Not connecting yet.");
    // Safe: do not allocate events, loopTick will early-return.
    return false;
  }

  // Allocate bounded HMS storage
  if (_events) {
    delete[] _events;
    _events = nullptr;
  }
  _events = new HmsEvent[_eventsCap];

  resetClient();

  webSerial.println("[MQTT] TLS: TOFU cert store enabled.");

#if defined(ARDUINO_ARCH_ESP32)
  if (!_parseQueue) {
    _parseQueue = xQueueCreate(4, sizeof(RawMsg));
  }
  if (!_pendingMutex) {
    _pendingMutex = xSemaphoreCreateMutex();
  }
  if (_parseQueue && !_parserTask) {
    xTaskCreatePinnedToCore(parserTaskThunk, "bb_mqtt_parse", 6144, this, 0, &_parserTask, 0);
  }
#endif

  _ready = true;

  if (!initClientFromSettings()) {
#if defined(ARDUINO_ARCH_ESP32)
    fetchCertSync("missing");
#endif
  }

  return true;
}

void BambuMqttClient::reloadFromSettings() {
  buildFromSettings();

  if (!configLooksValid()) {
    _ready = false;

    resetClient();

    // Free event buffer to keep state clean & avoid stale stuff
    if (_events) {
      delete[] _events;
      _events = nullptr;
    }

    webSerial.println("[MQTT] Settings reloaded but still incomplete.");
    return;
  }

  // Ensure event buffer exists
  if (_events) {
    delete[] _events;
    _events = nullptr;
  }
  _events = new HmsEvent[_eventsCap];

  _subscribed = false;
  _connected = false;
  _ready = true;

  resetClient();
  if (!initClientFromSettings()) {
#if defined(ARDUINO_ARCH_ESP32)
    fetchCertSync("missing");
#endif
  }

  webSerial.println("[MQTT] Settings reloaded.");
}

void BambuMqttClient::buildFromSettings() {
  // Always from settings. No alternatives.
  const char* ip  = _settings ? _settings->get.printerIP()  : "";
  const char* usn = _settings ? _settings->get.printerUSN() : "";
  const char* ac  = _settings ? _settings->get.printerAC()  : "";

  _printerIP   = (ip  && ip[0])  ? String(ip)  : "";
  _serial      = (usn && usn[0]) ? String(usn) : "";
  _accessCode  = (ac  && ac[0])  ? String(ac)  : "";

  _clientId = String("bambubeacon-") + String((uint32_t)ESP.getEfuseMac(), HEX);

  _topicReport  = String("device/") + _serial + "/report";
  _topicRequest = String("device/") + _serial + "/request";
  _serverUri    = String("mqtts://") + _printerIP + ":" + String(kPort);

  // HMS defaults (can be moved into settings later)
  _hmsTtlMs  = 20000;
  _eventsCap = 20;
  _ignoreNorm = "";

  // Do not touch _gcodeState here
}

bool BambuMqttClient::configLooksValid() const {
  return !_printerIP.isEmpty() && !_serial.isEmpty() && !_accessCode.isEmpty();
}

bool BambuMqttClient::timeIsValid() const {
  time_t now = time(nullptr);
  struct tm tmNow;
  if (!localtime_r(&now, &tmNow)) return false;
  return (tmNow.tm_year + 1900) >= 2022;
}

void BambuMqttClient::resetClient() {
  if (_client) {
    esp_mqtt_client_stop(_client);
    esp_mqtt_client_destroy(_client);
    _client = nullptr;
  }
  _clientStarted = false;
  _connected = false;
  _subscribed = false;
}

bool BambuMqttClient::initClientFromSettings() {
  if (!configLooksValid()) return false;
  if (!timeIsValid()) {
#if defined(ARDUINO_ARCH_ESP32)
    ensureTimeSync();
#endif
    return false;
  }
  const char* cert = _settings ? _settings->get.printerCert() : nullptr;
  if (!cert || !cert[0]) return false;

  esp_mqtt_client_config_t cfg = {};
  cfg.uri = _serverUri.c_str();
  cfg.client_id = _clientId.c_str();
  cfg.username = kUser;
  cfg.password = _accessCode.c_str();
  cfg.keepalive = 15;
  cfg.disable_auto_reconnect = false;
  cfg.buffer_size = 4096;
  cfg.user_context = this;
  cfg.event_handle = &BambuMqttClient::mqttEventHandler;
  cfg.cert_pem = cert;
  cfg.cert_len = 0;
  cfg.skip_cert_common_name_check = true;
  cfg.network_timeout_ms = kSocketTimeoutMs;

  _client = esp_mqtt_client_init(&cfg);
  if (!_client) {
    webSerial.println("[MQTT] Client init failed.");
    return false;
  }

  esp_mqtt_client_start(_client);
  _clientStarted = true;
  return true;
}

void BambuMqttClient::connect() {
  if (!_ready) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!configLooksValid()) {
    webSerial.println("[MQTT] Cannot connect: missing settings.");
    return;
  }

  if (!_client) {
#if defined(ARDUINO_ARCH_ESP32)
    fetchCertSync("connect");
#endif
    return;
  }
  if (_clientStarted) {
    esp_mqtt_client_reconnect(_client);
  } else {
    esp_mqtt_client_start(_client);
    _clientStarted = true;
  }
}

void BambuMqttClient::disconnect() {
  if (_client) esp_mqtt_client_disconnect(_client);
}

bool BambuMqttClient::isConnected() {
  return _connected;
}

void BambuMqttClient::loopTick() {
  // NEW: completely safe when not configured yet
  if (!_ready || !_events) return;

#if defined(ARDUINO_ARCH_ESP32)
  if (WiFi.status() == WL_CONNECTED) {
    ensureTimeSync();
  }

  if (!_client && configLooksValid() && _settings) {
    const char* cert = _settings->get.printerCert();
    if (cert && cert[0]) {
      if (timeIsValid()) {
        initClientFromSettings();
      }
    } else {
      fetchCertSync("missing");
    }
  }

  if (_pendingClientReset) {
    _pendingClientReset = false;
    resetClient();
    if (_clearStoredCert && _settings) {
      _settings->set.printerCert("");
      _settings->save();
      _clearStoredCert = false;
    }
    fetchCertSync("tls");
  }

  if (_certPendingSave && _fetchedCert) {
    _certPendingSave = false;
    if (_settings) {
      _settings->set.printerCert(String(_fetchedCert));
      _settings->save();
    }
    delete[] _fetchedCert;
    _fetchedCert = nullptr;
    _fetchedCertLen = 0;
    reloadFromSettings();
  }

  ParsedReport report;
  bool haveReport = false;
  if (_pendingMutex && _pendingReady) {
    if (xSemaphoreTake(_pendingMutex, 0) == pdTRUE) {
      if (_pendingReady) {
        report = _pendingReport;
        _pendingReady = false;
        haveReport = true;
      }
      xSemaphoreGive(_pendingMutex);
    }
  }
  if (haveReport) {
    applyParsedReport(report);
  }
#endif

  if (WiFi.status() != WL_CONNECTED) {
    // Still expire HMS so old errors do not stick forever if WiFi drops
    expireEvents(millis());
    return;
  }

  const uint32_t now = millis();
  if (isConnected() && now - _lastMqttDebugMs >= 10000UL) {
    const uint32_t age = _lastMsgMs ? (now - _lastMsgMs) : 0;
    webSerial.printf("[MQTT] Loop ok sub=%d lastMsgAge=%u ms lastLen=%u\n",
                     _subscribed ? 1 : 0, (unsigned)age, (unsigned)_lastMsgLen);
    _lastMqttDebugMs = now;
  }

#if defined(ARDUINO_ARCH_ESP32)
  if (now - _lastReportLogMs >= 5000UL) {
    uint32_t dropped = 0;
    if (_pendingMutex && xSemaphoreTake(_pendingMutex, 0) == pdTRUE) {
      dropped = _droppedMsgs;
      _droppedMsgs = 0;
      xSemaphoreGive(_pendingMutex);
    }
    if (dropped) {
      webSerial.printf("[MQTT] Dropped %u report(s) (queue full)\n", (unsigned)dropped);
      _lastReportLogMs = now;
    }
  }
#endif

  expireEvents(millis());
}

bool BambuMqttClient::publishRequest(const JsonDocument& doc, bool retain) {
  if (!_ready || !isConnected() || !_client) return false;

  String out;
  serializeJson(doc, out);

  const int msgId = esp_mqtt_client_enqueue(_client, _topicRequest.c_str(), out.c_str(),
                                            (int)out.length(), 0, retain ? 1 : 0, true);
  const bool ok = (msgId >= 0);
  webSerial.printf("[MQTT] Publish request ok=%d len=%u\n", ok ? 1 : 0, (unsigned)out.length());
  return ok;
}

void BambuMqttClient::onReport(ReportCallback cb) {
  _reportCb = cb;
}

const String& BambuMqttClient::topicReport() const { return _topicReport; }
const String& BambuMqttClient::topicRequest() const { return _topicRequest; }
String BambuMqttClient::gcodeState() const { return _gcodeState; }
uint8_t BambuMqttClient::printProgress() const { return _printProgress; }
uint8_t BambuMqttClient::downloadProgress() const { return _downloadProgress; }
float BambuMqttClient::bedTemp() const { return _bedTemp; }
float BambuMqttClient::bedTarget() const { return _bedTarget; }
bool BambuMqttClient::bedValid() const { return _bedValid; }
float BambuMqttClient::nozzleTemp() const { return _nozzleTemp; }
float BambuMqttClient::nozzleTarget() const { return _nozzleTarget; }
bool BambuMqttClient::nozzleValid() const { return _nozzleValid; }
bool BambuMqttClient::nozzleHeating() const { return _nozzleHeating; }

void BambuMqttClient::subscribeReportOnce() {
  if (_subscribed) return;

  webSerial.printf("[MQTT] Subscribing to %s\n", _topicReport.c_str());
  if (_client && isConnected()) {
    esp_mqtt_client_subscribe(_client, _topicReport.c_str(), 0);
  }
}

esp_err_t BambuMqttClient::mqttEventHandler(esp_mqtt_event_handle_t event) {
  if (!event || !event->user_context) return ESP_OK;
  return static_cast<BambuMqttClient*>(event->user_context)->handleEvent(event);
}

esp_err_t BambuMqttClient::handleEvent(esp_mqtt_event_handle_t event) {
  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      _connected = true;
      _subscribed = false;
      webSerial.println("[MQTT] Connected");
      subscribeReportOnce();
      break;
    case MQTT_EVENT_DISCONNECTED:
      _connected = false;
      _subscribed = false;
      if (_rxBuf) {
        delete[] _rxBuf;
        _rxBuf = nullptr;
        _rxLen = 0;
        _rxExpected = 0;
        _rxTopicMatch = false;
      }
      webSerial.println("[MQTT] Disconnected");
      break;
    case MQTT_EVENT_SUBSCRIBED:
      _subscribed = true;
      break;
    case MQTT_EVENT_DATA:
      handleMqttData(event);
      break;
    case MQTT_EVENT_ERROR:
      if (event->error_handle) {
        webSerial.printf("[MQTT] Error: type=%d tls=%d stack=%d sock=%d\n",
                         (int)event->error_handle->error_type,
                         (int)event->error_handle->esp_tls_last_esp_err,
                         (int)event->error_handle->esp_tls_stack_err,
                         (int)event->error_handle->esp_transport_sock_errno);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
          _pendingClientReset = true;
          if (!timeIsValid()) {
#if defined(ARDUINO_ARCH_ESP32)
            ensureTimeSync();
#endif
            _clearStoredCert = false;
          } else {
            _clearStoredCert = true;
          }
        }
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}

bool BambuMqttClient::topicMatches(const char* topic, int topicLen) const {
  if (!topic || topicLen <= 0) return false;
  if (_topicReport.isEmpty()) return false;
  const size_t expected = _topicReport.length();
  return (expected == (size_t)topicLen) && (memcmp(topic, _topicReport.c_str(), expected) == 0);
}

bool BambuMqttClient::handleMqttData(esp_mqtt_event_handle_t event) {
  if (!event || !event->data || event->data_len <= 0) return false;

  if (event->current_data_offset == 0) {
    _rxTopicMatch = topicMatches(event->topic, event->topic_len);
    if (_rxBuf) {
      delete[] _rxBuf;
      _rxBuf = nullptr;
    }
    _rxLen = 0;
    _rxExpected = 0;

    if (!_rxTopicMatch) return false;

    if (event->total_data_len <= 0) {
      _rxTopicMatch = false;
      return false;
    }

    _rxExpected = (size_t)event->total_data_len;
    if (_rxExpected > _maxPayloadSeen) _maxPayloadSeen = _rxExpected;

    size_t maxAllowed = kMqttBufferSize;
#if defined(ARDUINO_ARCH_ESP32)
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap > kMqttHeapSafety) {
      const size_t heapCap = freeHeap - kMqttHeapSafety;
      if (heapCap < maxAllowed) maxAllowed = heapCap;
    } else {
      maxAllowed = 0;
    }
#endif
    if (_rxExpected == 0 || _rxExpected > maxAllowed) {
      _droppedOversize++;
      if (_rxExpected > _maxPayloadDropped) _maxPayloadDropped = _rxExpected;
#if defined(ARDUINO_ARCH_ESP32)
      webSerial.printf("[MQTT] Drop payload len=%u max=%u free=%u\n",
                       (unsigned)_rxExpected, (unsigned)maxAllowed, (unsigned)freeHeap);
#else
      webSerial.printf("[MQTT] Drop payload len=%u max=%u\n",
                       (unsigned)_rxExpected, (unsigned)maxAllowed);
#endif
      _rxTopicMatch = false;
      _rxExpected = 0;
      return false;
    }

    _rxBuf = new (std::nothrow) uint8_t[_rxExpected];
    if (!_rxBuf) {
      _droppedAlloc++;
#if defined(ARDUINO_ARCH_ESP32)
      webSerial.printf("[MQTT] Drop payload len=%u (alloc failed, free=%u)\n",
                       (unsigned)_rxExpected, (unsigned)ESP.getFreeHeap());
#else
      webSerial.printf("[MQTT] Drop payload len=%u (alloc failed)\n",
                       (unsigned)_rxExpected);
#endif
      _rxTopicMatch = false;
      _rxExpected = 0;
      return false;
    }
  }

  if (!_rxTopicMatch || !_rxBuf) return false;
  if ((size_t)(event->current_data_offset + event->data_len) > _rxExpected) {
    delete[] _rxBuf;
    _rxBuf = nullptr;
    _rxLen = 0;
    _rxExpected = 0;
    _rxTopicMatch = false;
    return false;
  }

  memcpy(_rxBuf + event->current_data_offset, event->data, event->data_len);
  _rxLen = max(_rxLen, (size_t)(event->current_data_offset + event->data_len));

  if (_rxLen < _rxExpected) return true;

  _lastMsgLen = _rxExpected;
  _lastMsgMs = millis();

#if defined(ARDUINO_ARCH_ESP32)
  if (_parseQueue) {
    RawMsg msg;
    msg.payload = _rxBuf;
    msg.length = _rxExpected;
    _rxBuf = nullptr;
    _rxLen = 0;
    _rxExpected = 0;
    _rxTopicMatch = false;
    if (xQueueSend(_parseQueue, &msg, 0) != pdTRUE) {
      delete[] msg.payload;
      if (_pendingMutex && xSemaphoreTake(_pendingMutex, portMAX_DELAY) == pdTRUE) {
        _droppedMsgs++;
        xSemaphoreGive(_pendingMutex);
      } else {
        _droppedMsgs++;
      }
    }
    return true;
  }
#endif

  handleReportJson(_rxBuf, _rxExpected);
  delete[] _rxBuf;
  _rxBuf = nullptr;
  _rxLen = 0;
  _rxExpected = 0;
  _rxTopicMatch = false;
  return true;
}

void BambuMqttClient::handleReportJson(const uint8_t* payload, size_t length) {
  ParsedReport report;
  if (!parseReportJson(payload, length, report)) return;
  applyParsedReport(report);
}

bool BambuMqttClient::parseReportJson(const uint8_t* payload, size_t length, ParsedReport& out) {
  out = ParsedReport();
  out.nowMs = millis();

  static JsonDocument filter;
  if (filter.isNull()) {
    filter["print"]["gcode_state"] = true;
    filter["gcode_state"] = true;

    filter["print"]["mc_percent"] = true;
    filter["mc_percent"] = true;
    filter["print"]["percent"] = true;
    filter["percent"] = true;

    filter["print"]["download_progress"] = true;
    filter["print"]["download_percent"] = true;
    filter["print"]["dl_percent"] = true;
    filter["print"]["dl_progress"] = true;
    filter["print"]["prepare_per"] = true;
    filter["print"]["gcode_file_prepare_percent"] = true;
    filter["download_progress"] = true;
    filter["download_percent"] = true;

    filter["print"]["bed_temper"] = true;
    filter["print"]["bed_temperature"] = true;
    filter["bed_temper"] = true;
    filter["print"]["bed_target_temper"] = true;
    filter["print"]["bed_target_temperature"] = true;
    filter["bed_target_temper"] = true;

    filter["print"]["nozzle_temper"] = true;
    filter["nozzle_temper"] = true;
    filter["print"]["nozzle_target_temper"] = true;
    filter["nozzle_target_temper"] = true;

    filter["device"]["extruder"]["info"][0]["hnow"] = true;
    filter["device"]["extruder"]["info"][0]["htar"] = true;
    filter["device"]["extruder"]["info"][0]["temp"] = true;

    filter["hms"][0]["attr"] = true;
    filter["hms"][0]["code"] = true;
    filter["print"]["hms"][0]["attr"] = true;
    filter["print"]["hms"][0]["code"] = true;
    filter["data"]["hms"][0]["attr"] = true;
    filter["data"]["hms"][0]["code"] = true;
  }

  static JsonDocument doc;
  doc.clear();
  DeserializationError err = deserializeJson(doc, payload, length, DeserializationOption::Filter(filter));
  if (err) {
    webSerial.printf("[MQTT] JSON parse error: %s\n", err.c_str());
    return false;
  }

  if (doc["print"]["gcode_state"].is<const char*>()) {
    out.hasGcodeState = true;
    snprintf(out.gcodeState, sizeof(out.gcodeState), "%s", doc["print"]["gcode_state"].as<const char*>());
  } else if (doc["gcode_state"].is<const char*>()) {
    out.hasGcodeState = true;
    snprintf(out.gcodeState, sizeof(out.gcodeState), "%s", doc["gcode_state"].as<const char*>());
  }

  auto readInt = [](JsonVariant v, int& out) -> bool {
    if (v.is<int>()) { out = v.as<int>(); return true; }
    if (v.is<unsigned int>()) { out = (int)v.as<unsigned int>(); return true; }
    if (v.is<float>()) { out = (int)v.as<float>(); return true; }
    if (v.is<const char*>()) { out = atoi(v.as<const char*>()); return true; }
    return false;
  };

  auto readFloat = [](JsonVariant v, float& out) -> bool {
    if (v.is<float>()) { out = v.as<float>(); return true; }
    if (v.is<int>()) { out = (float)v.as<int>(); return true; }
    if (v.is<const char*>()) { out = (float)atof(v.as<const char*>()); return true; }
    return false;
  };

  int p = -1;
  if (readInt(doc["print"]["mc_percent"], p) ||
      readInt(doc["mc_percent"], p) ||
      readInt(doc["print"]["percent"], p) ||
      readInt(doc["percent"], p)) {
    if (p >= 0 && p <= 100) {
      out.hasPrintProgress = true;
      out.printProgress = (uint8_t)p;
    }
  }

  int dl = -1;
  if (readInt(doc["print"]["download_progress"], dl) ||
      readInt(doc["print"]["download_percent"], dl) ||
      readInt(doc["print"]["dl_percent"], dl) ||
      readInt(doc["print"]["dl_progress"], dl) ||
      readInt(doc["print"]["prepare_per"], dl) ||
      readInt(doc["print"]["gcode_file_prepare_percent"], dl) ||
      readInt(doc["download_progress"], dl) ||
      readInt(doc["download_percent"], dl)) {
    if (dl >= 0 && dl <= 100) {
      out.hasDownloadProgress = true;
      out.downloadProgress = (uint8_t)dl;
    }
  }

  float bed = 0.0f;
  float bedTarget = 0.0f;
  bool bedOk = readFloat(doc["print"]["bed_temper"], bed) ||
               readFloat(doc["print"]["bed_temperature"], bed) ||
               readFloat(doc["bed_temper"], bed);
  bool targetOk = readFloat(doc["print"]["bed_target_temper"], bedTarget) ||
                  readFloat(doc["print"]["bed_target_temperature"], bedTarget) ||
                  readFloat(doc["bed_target_temper"], bedTarget);
  if (bedOk && targetOk) {
    out.hasBed = true;
    out.bedTemp = bed;
    out.bedTarget = bedTarget;
  }

  float noz = 0.0f;
  float nozTarget = 0.0f;
  bool nozOk = readFloat(doc["print"]["nozzle_temper"], noz) ||
               readFloat(doc["nozzle_temper"], noz);
  bool nozTargetOk = readFloat(doc["print"]["nozzle_target_temper"], nozTarget) ||
                     readFloat(doc["nozzle_target_temper"], nozTarget);

  out.nozzleHeating = false;
  JsonVariant extr = doc["device"]["extruder"]["info"];
  if (extr.is<JsonArray>()) {
    for (JsonVariant v : extr.as<JsonArray>()) {
      if (!v.is<JsonObject>()) continue;
      JsonObject e = v.as<JsonObject>();

      int hnow = 0;
      if (readInt(e["hnow"], hnow) && hnow > 0) out.nozzleHeating = true;
      int htar = 0;
      if (readInt(e["htar"], htar) && htar > 0) out.nozzleHeating = true;

      float t = 0.0f;
      if (readFloat(e["temp"], t)) {
        if (t > 500.0f) {
          float fx = t / 65536.0f;
          if (fx >= 0.0f && fx <= 500.0f) t = fx;
        }
        if (!nozOk || t > noz) {
          noz = t;
          nozOk = true;
        }
      }
    }
  }

  if (nozOk) {
    out.hasNozzleTemp = true;
    out.nozzleTemp = noz;
  }
  if (nozTargetOk) {
    out.hasNozzleTarget = true;
    out.nozzleTarget = nozTarget;
  }

  JsonArray arr = findHmsArray(doc);
  if (arr) {
    out.hmsPresent = true;
    for (JsonVariant v : arr) {
      if (!v.is<JsonObject>()) continue;
      JsonObject h = v.as<JsonObject>();

      if (!h["attr"].is<uint32_t>() || !h["code"].is<uint32_t>()) continue;
      const uint32_t attr = (uint32_t)h["attr"];
      const uint32_t code = (uint32_t)h["code"];

      const uint64_t full = (uint64_t(attr) << 32) | uint64_t(code);
      char codeStr[24];
      formatHmsCodeStr(full, codeStr);
      if (isIgnored(codeStr)) continue;

      if (out.hmsCount < (sizeof(out.hms) / sizeof(out.hms[0]))) {
        out.hms[out.hmsCount].attr = attr;
        out.hms[out.hmsCount].code = code;
        out.hmsCount++;
      }
    }
  } else {
    out.hmsPresent = false;
  }

  return true;
}

void BambuMqttClient::applyParsedReport(const ParsedReport& report) {
  const uint32_t nowMs = report.nowMs ? report.nowMs : millis();

  if (report.hasGcodeState) {
    _gcodeState = report.gcodeState;
  }
  if (report.hasPrintProgress) {
    _printProgress = report.printProgress;
  }
  if (report.hasDownloadProgress) {
    _downloadProgress = report.downloadProgress;
  }
  if (report.hasBed) {
    _bedTemp = report.bedTemp;
    _bedTarget = report.bedTarget;
    _bedValid = true;
  }
  if (report.hasNozzleTemp) {
    _nozzleTemp = report.nozzleTemp;
    _nozzleValid = true;
  }
  if (report.hasNozzleTarget) {
    _nozzleTarget = report.nozzleTarget;
  }

  _nozzleHeating = report.nozzleHeating;

  if (report.hmsPresent) {
    for (uint8_t i = 0; i < report.hmsCount; i++) {
      upsertEvent(report.hms[i].attr, report.hms[i].code, nowMs);
    }
  }
  expireEvents(nowMs);

  logStatusIfNeeded(nowMs);

  if (_reportCb) _reportCb(nowMs);
}

#if defined(ARDUINO_ARCH_ESP32)
void BambuMqttClient::parserTask() {
  for (;;) {
    RawMsg msg;
    if (xQueueReceive(_parseQueue, &msg, portMAX_DELAY) != pdTRUE) continue;

    ParsedReport report;
    const bool ok = parseReportJson(msg.payload, msg.length, report);
    delete[] msg.payload;
    if (!ok) continue;

    if (_pendingMutex && xSemaphoreTake(_pendingMutex, portMAX_DELAY) == pdTRUE) {
      _pendingReport = report;
      _pendingReady = true;
      xSemaphoreGive(_pendingMutex);
    }
    vTaskDelay(1);
  }
}
#endif

#if defined(ARDUINO_ARCH_ESP32)
void BambuMqttClient::fetchCertSync(const char* reason) {
  if (_certFetchInProgress) return;
  if (!configLooksValid()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  const uint32_t now = millis();
  if (_lastCertFetchMs && (now - _lastCertFetchMs) < 60000UL) return;
  _lastCertFetchMs = now;

  webSerial.printf("[MQTT] Fetching printer cert (%s)\n", reason ? reason : "reason");
  _certFetchInProgress = true;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20);
  client.setHandshakeTimeout(20);

  webSerial.printf("[MQTT] Cert fetch connect %s:%u\n", _printerIP.c_str(), (unsigned)kPort);
  const bool ok = client.connect(_printerIP.c_str(), kPort);
  if (!ok) {
    char errBuf[128] = {0};
    const int err = client.lastError(errBuf, sizeof(errBuf));
    if (err) {
      webSerial.printf("[MQTT] Cert fetch connect failed (%d): %s\n", err, errBuf);
    } else {
      webSerial.println("[MQTT] Cert fetch connect failed.");
    }
    _certFetchInProgress = false;
    return;
  }

  const mbedtls_x509_crt* peer = client.getPeerCertificate();
  if (!peer || !peer->raw.p || peer->raw.len == 0) {
    webSerial.println("[MQTT] Cert fetch failed (no peer cert).");
    client.stop();
    _certFetchInProgress = false;
    return;
  }

  auto appendCertPem = [](const mbedtls_x509_crt* crt, String& out) -> bool {
    if (!crt || !crt->raw.p || crt->raw.len == 0) return false;
    const size_t pemBufLen = crt->raw.len * 2 + 512;
    uint8_t* pemBuf = new uint8_t[pemBufLen];
    if (!pemBuf) return false;
    size_t olen = 0;
    const int rc = mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
                                            "-----END CERTIFICATE-----\n",
                                            crt->raw.p, crt->raw.len,
                                            pemBuf, pemBufLen, &olen);
    if (rc == 0 && olen > 0) {
      out += reinterpret_cast<const char*>(pemBuf);
      delete[] pemBuf;
      return true;
    }
    delete[] pemBuf;
    return false;
  };

  String pemAll;
  int certCount = 0;
  int caCount = 0;
  for (const mbedtls_x509_crt* crt = peer; crt; crt = crt->next) {
    if (!crt->raw.p || crt->raw.len == 0) continue;
    certCount++;
    webSerial.printf("[MQTT] Cert raw len=%u ca=%d\n",
                     (unsigned)crt->raw.len, crt->ca_istrue ? 1 : 0);
    if (crt->ca_istrue) {
      if (appendCertPem(crt, pemAll)) {
        caCount++;
      }
    }
  }

  if (caCount == 0) {
    webSerial.println("[MQTT] Cert chain had no CA. Using leaf cert.");
    if (!appendCertPem(peer, pemAll)) {
      client.stop();
      _certFetchInProgress = false;
      return;
    }
  }

  if (pemAll.length() == 0) {
    client.stop();
    _certFetchInProgress = false;
    return;
  }

  if (_fetchedCert) {
    delete[] _fetchedCert;
    _fetchedCert = nullptr;
  }
  _fetchedCert = new char[pemAll.length() + 1];
  if (_fetchedCert) {
    memcpy(_fetchedCert, pemAll.c_str(), pemAll.length());
    _fetchedCert[pemAll.length()] = 0;
    _fetchedCertLen = pemAll.length();
    _certPendingSave = true;
    webSerial.printf("[MQTT] Cert fetched (%u bytes, %d certs, %d ca).\n",
                     (unsigned)_fetchedCertLen, certCount, caCount);
  } else {
    webSerial.println("[MQTT] Cert fetch failed (alloc).");
  }
  client.stop();
  _certFetchInProgress = false;
}
#endif

#if defined(ARDUINO_ARCH_ESP32)
void BambuMqttClient::ensureTimeSync() {
  if (_timeSyncOk) return;
  if (_timeSyncStarted) {
    if (timeIsValid()) {
      _timeSyncOk = true;
      webSerial.println("[MQTT] Time sync ok.");
    }
    return;
  }

  setenv("TZ", "UTC0", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  _timeSyncStarted = true;
  webSerial.println("[MQTT] Time sync started.");
}
#endif

JsonArray BambuMqttClient::findHmsArray(JsonDocument& doc) {
  if (doc["hms"].is<JsonArray>()) return doc["hms"].as<JsonArray>();
  if (doc["print"]["hms"].is<JsonArray>()) return doc["print"]["hms"].as<JsonArray>();
  if (doc["data"]["hms"].is<JsonArray>()) return doc["data"]["hms"].as<JsonArray>();
  return JsonArray();
}

bool BambuMqttClient::isIgnored(const char* codeStr) const {
  if (_ignoreNorm.isEmpty()) return false;
  return (_ignoreNorm.indexOf(codeStr) >= 0);
}

BambuMqttClient::Severity BambuMqttClient::severityFromCode(uint32_t code) {
  const uint16_t s = (uint16_t)(code >> 16);
  switch (s) {
    case 1: return Severity::Fatal;
    case 2: return Severity::Error;
    case 3: return Severity::Warning;
    case 4: return Severity::Info;
    default: return Severity::None;
  }
}

void BambuMqttClient::formatHmsCodeStr(uint64_t full, char out[24]) {
  const uint16_t a = (uint16_t)(full >> 48);
  const uint16_t b = (uint16_t)(full >> 32);
  const uint16_t c = (uint16_t)(full >> 16);
  const uint16_t d = (uint16_t)(full);
  snprintf(out, 24, "HMS_%04X_%04X_%04X_%04X", a, b, c, d);
}

void BambuMqttClient::upsertEvent(uint32_t attr, uint32_t code, uint32_t nowMs) {
  if (!_events) return;

  const uint64_t full = (uint64_t(attr) << 32) | uint64_t(code);

  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].full == full) {
      const bool wasActive = _events[i].active;
      _events[i].lastSeenMs = nowMs;
      _events[i].count++;
      _events[i].active = true;
      if (!wasActive) {
        char codeStr[24];
        formatHmsCodeStr(full, codeStr);
        webSerial.printf("[HMS] %s sev=%s\n", codeStr, severityToStr(severityFromCode(code)));
      }
      return;
    }
  }

  int slot = -1;
  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].full == 0) { slot = i; break; }
  }

  if (slot < 0) {
    uint32_t bestAge = 0;
    for (uint8_t i = 0; i < _eventsCap; i++) {
      if (_events[i].active) continue;
      const uint32_t age = nowMs - _events[i].lastSeenMs;
      if (age >= bestAge) { bestAge = age; slot = i; }
    }
    if (slot < 0) {
      bestAge = 0;
      for (uint8_t i = 0; i < _eventsCap; i++) {
        const uint32_t age = nowMs - _events[i].lastSeenMs;
        if (age >= bestAge) { bestAge = age; slot = i; }
      }
    }
  }

  HmsEvent& e = _events[slot];
  e.full = full;
  e.attr = attr;
  e.code = code;
  formatHmsCodeStr(full, e.codeStr);
  e.severity = severityFromCode(code);
  e.firstSeenMs = nowMs;
  e.lastSeenMs = nowMs;
  e.count = 1;
  e.active = true;
  webSerial.printf("[HMS] %s sev=%s\n", e.codeStr, severityToStr(e.severity));
}

void BambuMqttClient::expireEvents(uint32_t nowMs) {
  if (!_events) return;

  const uint32_t ttl = _hmsTtlMs ? _hmsTtlMs : 20000;

  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].full == 0) continue;
    if (_events[i].active && (nowMs - _events[i].lastSeenMs > ttl)) {
      _events[i].active = false;
    }
  }
}

BambuMqttClient::Severity BambuMqttClient::computeTopSeverity() const {
  Severity top = Severity::None;
  if (!_events) return top;

  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (!_events[i].active) continue;
    if ((uint8_t)_events[i].severity > (uint8_t)top) top = _events[i].severity;
  }
  return top;
}

BambuMqttClient::Severity BambuMqttClient::topSeverity() const {
  return computeTopSeverity();
}

bool BambuMqttClient::hasProblem() const {
  return (uint8_t)topSeverity() >= (uint8_t)Severity::Warning;
}

uint16_t BambuMqttClient::countActive(Severity sev) const {
  if (!_events) return 0;

  uint16_t n = 0;
  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].active && _events[i].severity == sev) n++;
  }
  return n;
}

uint16_t BambuMqttClient::countActiveTotal() const {
  if (!_events) return 0;

  uint16_t n = 0;
  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].active) n++;
  }
  return n;
}

size_t BambuMqttClient::getActiveEvents(HmsEvent* out, size_t maxOut) const {
  if (!_events || !out || !maxOut) return 0;

  size_t n = 0;
  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (!_events[i].active) continue;
    out[n++] = _events[i];
    if (n >= maxOut) break;
  }
  return n;
}

void BambuMqttClient::logStatusIfNeeded(uint32_t nowMs) {
  const Severity top = topSeverity();
  const uint16_t hmsCount = countActiveTotal();
  const bool stateChanged =
    (_gcodeState != _lastStatusState) ||
    (_printProgress != _lastStatusPrint) ||
    (_downloadProgress != _lastStatusDownload) ||
    (top != _lastStatusSeverity) ||
    (hmsCount != _lastStatusHmsCount);

  if (!stateChanged && (nowMs - _lastStatusLogMs < 5000UL)) return;

  const char* state = _gcodeState.length() ? _gcodeState.c_str() : "?";
  if (_bedValid) {
    webSerial.printf("[MQTT] State=%s Print=%u%% DL=%u%% Bed=%.1f/%.1f HMS=%u Top=%s\n",
                     state, _printProgress, _downloadProgress,
                     _bedTemp, _bedTarget, (unsigned)hmsCount, severityToStr(top));
  } else {
    webSerial.printf("[MQTT] State=%s Print=%u%% DL=%u%% Bed=n/a HMS=%u Top=%s\n",
                     state, _printProgress, _downloadProgress,
                     (unsigned)hmsCount, severityToStr(top));
  }

  _lastStatusLogMs = nowMs;
  _lastStatusState = _gcodeState;
  _lastStatusPrint = _printProgress;
  _lastStatusDownload = _downloadProgress;
  _lastStatusSeverity = top;
  _lastStatusHmsCount = hmsCount;
}
