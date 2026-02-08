#include "BambuMqttClient.h"
#include <mbedtls/pem.h>
#include <mbedtls/x509_crt.h>
#include <ctype.h>
#include <errno.h>

namespace {
constexpr uint32_t kSocketTimeoutMs = 15000;
constexpr uint32_t kHardResetMinIntervalMs = 30000;
constexpr uint32_t kTransportErrWindowMs = 20000;
constexpr uint32_t kReconnectKickIntervalMs = 7000;
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

static String normalizeIgnoreList(const char* raw) {
  String out;
  if (!raw || !raw[0]) return out;
  String token;
  for (const char* p = raw; *p; ++p) {
    char c = *p;
    if (c == '\r' || c == '\n' || c == ',' || c == ';' || isspace((unsigned char)c)) {
      if (token.length()) {
        token.toUpperCase();
        out += "\n";
        out += token;
        out += "\n";
        token = "";
      }
      continue;
    }
    token += (char)toupper((unsigned char)c);
  }
  if (token.length()) {
    token.toUpperCase();
    out += "\n";
    out += token;
    out += "\n";
  }
  return out;
}

const char* BambuMqttClient::kUser = "bblp";

BambuMqttClient::BambuMqttClient() {}
BambuMqttClient::~BambuMqttClient() {
  if (_pendingMutex) {
    vSemaphoreDelete(_pendingMutex);
    _pendingMutex = nullptr;
  }
  if (_fetchedCert) {
    delete[] _fetchedCert;
    _fetchedCert = nullptr;
  }
  if (_client) {
    esp_mqtt_client_stop(_client);
    esp_mqtt_client_destroy(_client);
    _client = nullptr;
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

  if (!_pendingMutex) {
    _pendingMutex = xSemaphoreCreateMutex();
  }

  _ready = true;

  if (!initClientFromSettings()) {
    fetchCertSync("missing");
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
    fetchCertSync("missing");
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
  const char* ignoreRaw = _settings ? _settings->get.hmsIgnore() : "";
  _ignoreNorm = normalizeIgnoreList(ignoreRaw);

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
    ensureTimeSync();
    return false;
  }
  const char* cert = _settings ? _settings->get.printerCert() : nullptr;
  if (!cert || !cert[0]) return false;

  esp_mqtt_client_config_t cfg = {};
  cfg.uri = _serverUri.c_str();
  cfg.client_id = _clientId.c_str();
  cfg.username = kUser;
  cfg.password = _accessCode.c_str();
  cfg.keepalive = 20;
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
    fetchCertSync("connect");
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
    if (_resetNeedsCertFetch) {
      _resetNeedsCertFetch = false;
      fetchCertSync("tls");
    } else {
      if (!initClientFromSettings()) {
        fetchCertSync("tls");
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED && _client && !_connected &&
      !_pendingClientReset && !_certFetchInProgress) {
    const uint32_t nowMs = millis();
    if (_lastReconnectKickMs == 0 ||
        (uint32_t)(nowMs - _lastReconnectKickMs) >= kReconnectKickIntervalMs) {
      _lastReconnectKickMs = nowMs;
      connect();
    }
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

  if (WiFi.status() != WL_CONNECTED) {
    // Still expire HMS so old errors do not stick forever if WiFi drops
    expireEvents(millis());
    return;
  }

  const uint32_t now = millis();

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
      _lastReconnectKickMs = 0;
      _transportErrWindowStartMs = 0;
      _transportErrCount = 0;
      _resetNeedsCertFetch = false;
      webSerial.println("[MQTT] Connected");
      subscribeReportOnce();
      break;
    case MQTT_EVENT_DISCONNECTED:
      _connected = false;
      _subscribed = false;
      _rxExpected = 0;
      _rxReceived = 0;
      _rxTopicMatch = false;
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
        const int errType = (int)event->error_handle->error_type;
        const int tlsErr = (int)event->error_handle->esp_tls_last_esp_err;
        const int stackErr = (int)event->error_handle->esp_tls_stack_err;
        const int sockErr = (int)event->error_handle->esp_transport_sock_errno;
        webSerial.printf("[MQTT] Error: type=%d tls=%d stack=%d sock=%d\n",
                         errType, tlsErr, stackErr, sockErr);
        if (errType == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
          const bool transientSockErr =
              (sockErr == EAGAIN) || (sockErr == EWOULDBLOCK) ||
              (sockErr == EINTR) || (sockErr == ETIMEDOUT);

          // Typical transient socket timeout on unstable links: let built-in
          // reconnect handle it without expensive cert refresh/reset.
          if (transientSockErr && tlsErr == 0 && stackErr == 0) {
            break;
          }

          const uint32_t nowMs = millis();
          if (_transportErrWindowStartMs == 0 ||
              (uint32_t)(nowMs - _transportErrWindowStartMs) > kTransportErrWindowMs) {
            _transportErrWindowStartMs = nowMs;
            _transportErrCount = 1;
          } else if (_transportErrCount < 255) {
            _transportErrCount++;
          }

          const bool likelyTlsIssue = (tlsErr != 0 || stackErr != 0);
          const bool tooManyTransportErrors = (_transportErrCount >= 3);
          const bool hardResetAllowed =
              (_lastHardResetMs == 0) ||
              ((uint32_t)(nowMs - _lastHardResetMs) >= kHardResetMinIntervalMs);

          if (!hardResetAllowed) {
            break;
          }
          if (!likelyTlsIssue && !tooManyTransportErrors) {
            break;
          }

          _lastHardResetMs = nowMs;
          _pendingClientReset = true;
          _resetNeedsCertFetch = likelyTlsIssue;
          _clearStoredCert = false;
          if (!timeIsValid()) {
            ensureTimeSync();
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
    _rxExpected = 0;
    _rxReceived = 0;

    if (!_rxTopicMatch) return false;

    if (event->total_data_len <= 0) {
      _rxTopicMatch = false;
      return false;
    }

    _rxExpected = (size_t)event->total_data_len;
    _streamParser.reset();
  }

  if (!_rxTopicMatch) return false;
  if ((size_t)(event->current_data_offset + event->data_len) > _rxExpected) {
    _rxExpected = 0;
    _rxReceived = 0;
    _rxTopicMatch = false;
    return false;
  }

  if (!_streamParser.feed(reinterpret_cast<const uint8_t*>(event->data), (size_t)event->data_len)) {
    _parseFail = (uint8_t)(_parseFail + 1);
    _rxExpected = 0;
    _rxReceived = 0;
    _rxTopicMatch = false;
    return false;
  }
  _rxReceived = max(_rxReceived, (size_t)(event->current_data_offset + event->data_len));

  if (_rxReceived < _rxExpected) return true;

  _lastMsgLen = _rxExpected;
  _lastMsgMs = millis();

  ParsedReport report;
  if (_streamParser.finish(report)) {
    _parseOk = (uint8_t)(_parseOk + 1);
    if (_pendingMutex && xSemaphoreTake(_pendingMutex, portMAX_DELAY) == pdTRUE) {
      _pendingReport = report;
      _pendingReady = true;
      xSemaphoreGive(_pendingMutex);
    } else {
      applyParsedReport(report);
    }
  } else {
    _parseFail = (uint8_t)(_parseFail + 1);
  }

  _rxExpected = 0;
  _rxReceived = 0;
  _rxTopicMatch = false;
  return true;
}

/* ================= Streaming JSON Parser (no heap alloc) ================= */

BambuMqttClient::StreamParser::KeyId
BambuMqttClient::StreamParser::keyIdFromString(const char* s, size_t len) {
  if (!s || len == 0) return KeyId::Unknown;
  switch (len) {
    case 3:
      if (memcmp(s, "hms", 3) == 0) return KeyId::Hms;
      break;
    case 4:
      if (memcmp(s, "info", 4) == 0) return KeyId::Info;
      if (memcmp(s, "attr", 4) == 0) return KeyId::Attr;
      if (memcmp(s, "code", 4) == 0) return KeyId::Code;
      if (memcmp(s, "data", 4) == 0) return KeyId::Data;
      if (memcmp(s, "temp", 4) == 0) return KeyId::Temp;
      if (memcmp(s, "htar", 4) == 0) return KeyId::Htar;
      if (memcmp(s, "hnow", 4) == 0) return KeyId::Hnow;
      break;
    case 5:
      if (memcmp(s, "print", 5) == 0) return KeyId::Print;
      break;
    case 6:
      if (memcmp(s, "device", 6) == 0) return KeyId::Device;
      break;
    case 7:
      if (memcmp(s, "percent", 7) == 0) return KeyId::Percent;
      break;
    case 8:
      if (memcmp(s, "extruder", 8) == 0) return KeyId::Extruder;
      break;
    case 10:
      if (memcmp(s, "mc_percent", 10) == 0) return KeyId::McPercent;
      if (memcmp(s, "dl_percent", 10) == 0) return KeyId::DlPercent;
      if (memcmp(s, "bed_temper", 10) == 0) return KeyId::BedTemper;
      break;
    case 11:
      if (memcmp(s, "gcode_state", 11) == 0) return KeyId::GcodeState;
      if (memcmp(s, "dl_progress", 11) == 0) return KeyId::DlProgress;
      if (memcmp(s, "prepare_per", 11) == 0) return KeyId::PreparePer;
      break;
    case 13:
      if (memcmp(s, "nozzle_temper", 13) == 0) return KeyId::NozzleTemper;
      break;
    case 15:
      if (memcmp(s, "bed_temperature", 15) == 0) return KeyId::BedTemperature;
      break;
    case 16:
      if (memcmp(s, "download_percent", 16) == 0) return KeyId::DownloadPercent;
      break;
    case 17:
      if (memcmp(s, "bed_target_temper", 17) == 0) return KeyId::BedTargetTemper;
      break;
    case 18:
      if (memcmp(s, "download_progress", 18) == 0) return KeyId::DownloadProgress;
      break;
    case 20:
      if (memcmp(s, "nozzle_target_temper", 20) == 0) return KeyId::NozzleTargetTemper;
      break;
    case 22:
      if (memcmp(s, "bed_target_temperature", 22) == 0) return KeyId::BedTargetTemperature;
      break;
    case 26:
      if (memcmp(s, "gcode_file_prepare_percent", 26) == 0) return KeyId::GcodeFilePreparePercent;
      break;
    default:
      break;
  }
  return KeyId::Unknown;
}

void BambuMqttClient::StreamParser::reset() {
  _mode = Mode::Default;
  _escape = false;
  _strLen = 0;
  _numLen = 0;
  _litLen = 0;
  _currentKey = KeyId::Unknown;
  _error = false;
  _depth = 0;
  _report = ParsedReport();
  _report.nowMs = millis();
  _bedOk = false;
  _bedTargetOk = false;
  _bedTemp = 0.0f;
  _bedTarget = 0.0f;
  _nozOk = false;
  _nozTargetOk = false;
  _nozTemp = 0.0f;
  _nozTarget = 0.0f;
  _nozzleHeatingCandidate = false;
  _hmsArraySeen = false;
  _hmsAttr = 0;
  _hmsCode = 0;
  _hmsAttrSet = false;
  _hmsCodeSet = false;
}

BambuMqttClient::StreamParser::KeyId
BambuMqttClient::StreamParser::parentKey() const {
  if (_depth <= 0) return KeyId::Root;
  return _stack[_depth - 1].key;
}

BambuMqttClient::StreamParser::KeyId
BambuMqttClient::StreamParser::grandParentKey() const {
  if (_depth <= 1) return KeyId::Root;
  return _stack[_depth - 2].key;
}

bool BambuMqttClient::StreamParser::inExtruderInfoArray() const {
  for (int i = _depth - 1; i >= 0; --i) {
    if (_stack[i].isArray && _stack[i].isExtruderInfoArray) return true;
  }
  return false;
}

int BambuMqttClient::StreamParser::currentExtruderInfoIndex() const {
  for (int i = _depth - 1; i >= 0; --i) {
    if (_stack[i].isArray && _stack[i].isExtruderInfoArray) return _stack[i].index;
  }
  return -1;
}

bool BambuMqttClient::StreamParser::inHmsItem() const {
  if (_depth <= 0) return false;
  if (_stack[_depth - 1].isArray) return false;
  return _stack[_depth - 1].isHmsItem;
}

void BambuMqttClient::StreamParser::pushObject() {
  if (_depth >= (int)(sizeof(_stack) / sizeof(_stack[0]))) {
    _error = true;
    return;
  }
  if (_depth > 0 && _stack[_depth - 1].isArray && _stack[_depth - 1].expectingValue) {
    _stack[_depth - 1].index++;
    _stack[_depth - 1].expectingValue = false;
  }
  Ctx ctx;
  ctx.isArray = false;
  ctx.expectingKey = true;
  ctx.expectingValue = false;
  ctx.key = (_depth == 0) ? KeyId::Root : _currentKey;
  ctx.isHmsArray = false;
  ctx.isExtruderInfoArray = false;
  ctx.isHmsItem = (_depth > 0 && _stack[_depth - 1].isArray && _stack[_depth - 1].isHmsArray);
  ctx.index = -1;
  if (ctx.isHmsItem) {
    _hmsAttrSet = false;
    _hmsCodeSet = false;
  }
  _stack[_depth++] = ctx;
  _currentKey = KeyId::Unknown;
}

void BambuMqttClient::StreamParser::pushArray() {
  if (_depth >= (int)(sizeof(_stack) / sizeof(_stack[0]))) {
    _error = true;
    return;
  }
  if (_depth > 0 && _stack[_depth - 1].isArray && _stack[_depth - 1].expectingValue) {
    _stack[_depth - 1].index++;
    _stack[_depth - 1].expectingValue = false;
  }
  Ctx ctx;
  ctx.isArray = true;
  ctx.expectingKey = false;
  ctx.expectingValue = true;
  ctx.key = (_depth == 0) ? KeyId::Root : _currentKey;
  const KeyId p = parentKey();
  const KeyId gp = grandParentKey();
  ctx.isHmsArray = (ctx.key == KeyId::Hms) && (p == KeyId::Print || p == KeyId::Data || p == KeyId::Root);
  ctx.isExtruderInfoArray = (ctx.key == KeyId::Info) && (p == KeyId::Extruder) && (gp == KeyId::Device);
  ctx.isHmsItem = false;
  ctx.index = -1;
  if (ctx.isHmsArray) _hmsArraySeen = true;
  _stack[_depth++] = ctx;
  _currentKey = KeyId::Unknown;
}

void BambuMqttClient::StreamParser::addHmsIfReady() {
  if (!_hmsAttrSet || !_hmsCodeSet) return;
  if (_report.hmsCount >= (sizeof(_report.hms) / sizeof(_report.hms[0]))) return;
  _report.hms[_report.hmsCount].attr = _hmsAttr;
  _report.hms[_report.hmsCount].code = _hmsCode;
  _report.hmsCount++;
}

void BambuMqttClient::StreamParser::popContext() {
  if (_depth <= 0) return;
  Ctx ctx = _stack[_depth - 1];
  _depth--;
  if (!ctx.isArray && ctx.isHmsItem) {
    addHmsIfReady();
  }
}

void BambuMqttClient::StreamParser::valueCompleted() {
  if (_depth <= 0) return;
  if (_stack[_depth - 1].isArray) {
    // no-op, array index not tracked
  } else {
    _stack[_depth - 1].expectingKey = true;
  }
  _currentKey = KeyId::Unknown;
}

bool BambuMqttClient::StreamParser::isNumberChar(char c) const {
  return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

bool BambuMqttClient::StreamParser::parseInt(const char* s, size_t len, int& out) const {
  if (!s || len == 0) return false;
  char buf[24];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, s, len);
  buf[len] = 0;
  char* endp = nullptr;
  long v = strtol(buf, &endp, 10);
  if (endp == buf) return false;
  out = (int)v;
  return true;
}

bool BambuMqttClient::StreamParser::parseFloat(const char* s, size_t len, float& out) const {
  if (!s || len == 0) return false;
  char buf[24];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, s, len);
  buf[len] = 0;
  char* endp = nullptr;
  float v = strtof(buf, &endp);
  if (endp == buf) return false;
  out = v;
  return true;
}

void BambuMqttClient::StreamParser::handleValueString(const char* s, size_t len) {
  const KeyId p = parentKey();
  const bool inPrint = (p == KeyId::Print);
  const bool atRoot = (p == KeyId::Root);

  if (_currentKey == KeyId::GcodeState && (inPrint || atRoot)) {
    _report.hasGcodeState = true;
    char buf[sizeof(_report.gcodeState)];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    snprintf(_report.gcodeState, sizeof(_report.gcodeState), "%s", buf);
    return;
  }

  if (_currentKey == KeyId::McPercent || _currentKey == KeyId::Percent ||
      _currentKey == KeyId::DownloadProgress || _currentKey == KeyId::DownloadPercent ||
      _currentKey == KeyId::DlPercent || _currentKey == KeyId::DlProgress ||
      _currentKey == KeyId::PreparePer || _currentKey == KeyId::GcodeFilePreparePercent ||
      _currentKey == KeyId::BedTemper || _currentKey == KeyId::BedTemperature ||
      _currentKey == KeyId::BedTargetTemper || _currentKey == KeyId::BedTargetTemperature ||
      _currentKey == KeyId::NozzleTemper || _currentKey == KeyId::NozzleTargetTemper ||
      _currentKey == KeyId::Attr || _currentKey == KeyId::Code ||
      _currentKey == KeyId::Hnow || _currentKey == KeyId::Htar || _currentKey == KeyId::Temp) {
    handleValueNumber(s, len);
  }
}

void BambuMqttClient::StreamParser::handleValueNumber(const char* s, size_t len) {
  const KeyId p = parentKey();
  const bool inPrint = (p == KeyId::Print);
  const bool atRoot = (p == KeyId::Root);

  if (_currentKey == KeyId::McPercent || _currentKey == KeyId::Percent) {
    if (inPrint || atRoot) {
      int v = 0;
      if (parseInt(s, len, v) && v >= 0 && v <= 100) {
        _report.hasPrintProgress = true;
        _report.printProgress = (uint8_t)v;
      }
    }
    return;
  }

  if (_currentKey == KeyId::DownloadProgress || _currentKey == KeyId::DownloadPercent ||
      _currentKey == KeyId::DlPercent || _currentKey == KeyId::DlProgress ||
      _currentKey == KeyId::PreparePer || _currentKey == KeyId::GcodeFilePreparePercent) {
    if (inPrint || atRoot) {
      int v = 0;
      if (parseInt(s, len, v) && v >= 0 && v <= 100) {
        _report.hasDownloadProgress = true;
        _report.downloadProgress = (uint8_t)v;
      }
    }
    return;
  }

  if (_currentKey == KeyId::BedTemper || _currentKey == KeyId::BedTemperature) {
    if (inPrint || atRoot) {
      float v = 0.0f;
      if (parseFloat(s, len, v)) {
        _bedTemp = v;
        _bedOk = true;
      }
    }
    return;
  }

  if (_currentKey == KeyId::BedTargetTemper || _currentKey == KeyId::BedTargetTemperature) {
    if (inPrint || atRoot) {
      float v = 0.0f;
      if (parseFloat(s, len, v)) {
        _bedTarget = v;
        _bedTargetOk = true;
      }
    }
    return;
  }

  if (_currentKey == KeyId::NozzleTemper) {
    if (inPrint || atRoot) {
      float v = 0.0f;
      if (parseFloat(s, len, v)) {
        _nozTemp = v;
        _nozOk = true;
      }
    }
    return;
  }

  if (_currentKey == KeyId::NozzleTargetTemper) {
    if (inPrint || atRoot) {
      float v = 0.0f;
      if (parseFloat(s, len, v)) {
        _nozTarget = v;
        _nozTargetOk = true;
      }
    }
    return;
  }

  if (inHmsItem()) {
    int v = 0;
    if ((_currentKey == KeyId::Attr || _currentKey == KeyId::Code) && parseInt(s, len, v) && v >= 0) {
      if (_currentKey == KeyId::Attr) {
        _hmsAttr = (uint32_t)v;
        _hmsAttrSet = true;
      } else {
        _hmsCode = (uint32_t)v;
        _hmsCodeSet = true;
      }
    }
    return;
  }

  if (inExtruderInfoArray()) {
    if (_currentKey == KeyId::Hnow || _currentKey == KeyId::Htar) {
      int v = 0;
      if (parseInt(s, len, v) && v > 0) _nozzleHeatingCandidate = true;
      return;
    }
    if (_currentKey == KeyId::Temp) {
      float t = 0.0f;
      if (parseFloat(s, len, t)) {
        if (t > 500.0f) {
          float fx = t / 65536.0f;
          if (fx >= 0.0f && fx <= 500.0f) t = fx;
        }
        if (!_nozOk || t > _nozTemp) {
          _nozTemp = t;
          _nozOk = true;
        }
      }
      return;
    }
  }
}

void BambuMqttClient::StreamParser::handleValueLiteral(const char* s, size_t len) {
  (void)s;
  (void)len;
}

bool BambuMqttClient::StreamParser::feed(const uint8_t* data, size_t len) {
  if (_error) return false;
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (_mode == Mode::InNumber) {
      if (isNumberChar(c)) {
        if (_numLen < sizeof(_numBuf) - 1) _numBuf[_numLen++] = c;
        continue;
      }
      _numBuf[_numLen] = 0;
      handleValueNumber(_numBuf, _numLen);
      valueCompleted();
      _numLen = 0;
      _mode = Mode::Default;
      i--;
      continue;
    }
    if (_mode == Mode::InLiteral) {
      if (isalpha((unsigned char)c)) {
        if (_litLen < sizeof(_litBuf) - 1) _litBuf[_litLen++] = c;
        continue;
      }
      _litBuf[_litLen] = 0;
      handleValueLiteral(_litBuf, _litLen);
      valueCompleted();
      _litLen = 0;
      _mode = Mode::Default;
      i--;
      continue;
    }
    if (_mode == Mode::InStringKey || _mode == Mode::InStringVal) {
      if (_escape) {
        _escape = false;
        if (_strLen < sizeof(_strBuf) - 1) _strBuf[_strLen++] = c;
        continue;
      }
      if (c == '\\') {
        _escape = true;
        continue;
      }
      if (c == '"') {
        _strBuf[_strLen] = 0;
        if (_mode == Mode::InStringKey) {
          _currentKey = keyIdFromString(_strBuf, _strLen);
        } else {
          handleValueString(_strBuf, _strLen);
          valueCompleted();
        }
        _strLen = 0;
        _mode = Mode::Default;
        continue;
      }
      if (_strLen < sizeof(_strBuf) - 1) _strBuf[_strLen++] = c;
      continue;
    }

    if (isspace((unsigned char)c)) continue;

    switch (c) {
      case '{':
        pushObject();
        break;
      case '[':
        pushArray();
        break;
      case '}':
        popContext();
        valueCompleted();
        break;
      case ']':
        popContext();
        valueCompleted();
        break;
      case '"': {
        if (_depth > 0 && _stack[_depth - 1].isArray && _stack[_depth - 1].expectingValue) {
          _stack[_depth - 1].index++;
          _stack[_depth - 1].expectingValue = false;
        }
        if (_depth > 0 && !_stack[_depth - 1].isArray && _stack[_depth - 1].expectingKey) {
          _mode = Mode::InStringKey;
        } else {
          _mode = Mode::InStringVal;
        }
        _strLen = 0;
        _escape = false;
        break;
      }
      case ':':
        if (_depth > 0 && !_stack[_depth - 1].isArray) {
          _stack[_depth - 1].expectingKey = false;
        }
        break;
      case ',':
        if (_depth > 0) {
          if (_stack[_depth - 1].isArray) {
            _stack[_depth - 1].expectingValue = true;
          } else {
            _stack[_depth - 1].expectingKey = true;
          }
        }
        break;
      default:
        if (isNumberChar(c)) {
          if (_depth > 0 && _stack[_depth - 1].isArray && _stack[_depth - 1].expectingValue) {
            _stack[_depth - 1].index++;
            _stack[_depth - 1].expectingValue = false;
          }
          _mode = Mode::InNumber;
          _numLen = 0;
          _numBuf[_numLen++] = c;
        } else if (c == 't' || c == 'f' || c == 'n') {
          if (_depth > 0 && _stack[_depth - 1].isArray && _stack[_depth - 1].expectingValue) {
            _stack[_depth - 1].index++;
            _stack[_depth - 1].expectingValue = false;
          }
          _mode = Mode::InLiteral;
          _litLen = 0;
          _litBuf[_litLen++] = c;
        }
        break;
    }
  }
  return !_error;
}

bool BambuMqttClient::StreamParser::finish(ParsedReport& out) {
  if (_error) return false;
  if (_mode == Mode::InNumber) {
    _numBuf[_numLen] = 0;
    handleValueNumber(_numBuf, _numLen);
    _numLen = 0;
    _mode = Mode::Default;
  } else if (_mode == Mode::InLiteral) {
    _litBuf[_litLen] = 0;
    handleValueLiteral(_litBuf, _litLen);
    _litLen = 0;
    _mode = Mode::Default;
  }

  if (_bedOk && _bedTargetOk) {
    _report.hasBed = true;
    _report.bedTemp = _bedTemp;
    _report.bedTarget = _bedTarget;
  }
  if (_nozOk) {
    _report.hasNozzleTemp = true;
    _report.nozzleTemp = _nozTemp;
  }
  if (_nozTargetOk) {
    _report.hasNozzleTarget = true;
    _report.nozzleTarget = _nozTarget;
  }
  if (_nozOk && _nozTargetOk) {
    _report.nozzleHeating = (_nozTarget > (_nozTemp + 2.0f));
  } else {
    _report.nozzleHeating = _nozzleHeatingCandidate;
  }
  _report.hmsPresent = _hmsArraySeen;

  out = _report;
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
      const uint64_t full = (uint64_t(report.hms[i].attr) << 32) | uint64_t(report.hms[i].code);
      char codeStr[24];
      formatHmsCodeStr(full, codeStr);
      if (isIgnored(codeStr)) continue;
      upsertEvent(report.hms[i].attr, report.hms[i].code, nowMs);
    }
  }
  expireEvents(nowMs);

  logStatusIfNeeded(nowMs);

  if (_reportCb) _reportCb(nowMs);
}

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

bool BambuMqttClient::isIgnored(const char* codeStr) const {
  if (_ignoreNorm.isEmpty()) return false;
  String needle = "\n";
  needle += codeStr;
  needle += "\n";
  return (_ignoreNorm.indexOf(needle) >= 0);
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
    webSerial.printf("[MQTT] State=%s Print=%u%% DL=%u%% Bed=%.1f/%.1f HMS=%u Top=%s Len=%u Parse=%u/%u\n",
                     state, _printProgress, _downloadProgress,
                     _bedTemp, _bedTarget, (unsigned)hmsCount, severityToStr(top),
                     (unsigned)_lastMsgLen,
                     (unsigned)_parseOk, (unsigned)_parseFail);
  } else {
    webSerial.printf("[MQTT] State=%s Print=%u%% DL=%u%% Bed=n/a HMS=%u Top=%s Len=%u Parse=%u/%u\n",
                     state, _printProgress, _downloadProgress,
                     (unsigned)hmsCount, severityToStr(top),
                     (unsigned)_lastMsgLen,
                     (unsigned)_parseOk, (unsigned)_parseFail);
  }

  _lastStatusLogMs = nowMs;
  _lastStatusState = _gcodeState;
  _lastStatusPrint = _printProgress;
  _lastStatusDownload = _downloadProgress;
  _lastStatusSeverity = top;
  _lastStatusHmsCount = hmsCount;
}
