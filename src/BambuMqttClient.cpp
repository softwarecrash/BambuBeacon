#include "BambuMqttClient.h"

namespace {
BambuMqttClient* s_instance = nullptr;
constexpr uint32_t kTlsHandshakeTimeoutMs = 5000;
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
void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
  if (s_instance) s_instance->handleMqttMessage(topic, payload, length);
}
}

const char* BambuMqttClient::kUser = "bblp";

BambuMqttClient::BambuMqttClient()
: _net(),
  _mqtt(_net) {}
BambuMqttClient::~BambuMqttClient() {
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

  _net.setInsecure();
#if defined(ARDUINO_ARCH_ESP32)
  _net.setHandshakeTimeout(kTlsHandshakeTimeoutMs);
#endif
  _net.setTimeout(kSocketTimeoutMs);
  _mqtt.setServer(_printerIP.c_str(), kPort);
  _mqtt.setBufferSize(kMqttBufferSize);

  s_instance = this;
  _mqtt.setCallback(mqttCallback);

  webSerial.println("[MQTT] TLS: insecure mode enabled.");

  _ready = true;

  if (WiFi.status() == WL_CONNECTED) {
    connect();
  } else {
    webSerial.println("[MQTT] WiFi not connected yet - will connect from loopTick().");
  }

  return true;
}

void BambuMqttClient::reloadFromSettings() {
  buildFromSettings();

  if (!configLooksValid()) {
    _ready = false;

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

  _net.setInsecure();
#if defined(ARDUINO_ARCH_ESP32)
  _net.setHandshakeTimeout(kTlsHandshakeTimeoutMs);
#endif
  _net.setTimeout(kSocketTimeoutMs);
  _mqtt.setServer(_printerIP.c_str(), kPort);
  _mqtt.setBufferSize(kMqttBufferSize);

  _subscribed = false;
  _ready = true;

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

void BambuMqttClient::connect() {
  if (!_ready) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!configLooksValid()) {
    webSerial.println("[MQTT] Cannot connect: missing settings.");
    return;
  }

  if (_mqtt.connected()) return;

  webSerial.printf("[MQTT] Connecting to %s (clientId=%s)\n",
                   _serverUri.c_str(), _clientId.c_str());
  const bool ok = _mqtt.connect(_clientId.c_str(), kUser, _accessCode.c_str());
  if (ok) {
    webSerial.println("[MQTT] Connected");
    _subscribed = false;
    subscribeReportOnce();
  } else {
    webSerial.printf("[MQTT] Connect failed (state=%d)\n", _mqtt.state());
  }
}

void BambuMqttClient::disconnect() {
  _mqtt.disconnect();
}

bool BambuMqttClient::isConnected() {
  return _mqtt.connected();
}

void BambuMqttClient::loopTick() {
  // NEW: completely safe when not configured yet
  if (!_ready || !_events) return;
  if (WiFi.status() != WL_CONNECTED) {
    // Still expire HMS so old errors do not stick forever if WiFi drops
    expireEvents(millis());
    return;
  }

  if (!_mqtt.connected()) {
    _subscribed = false;
    const uint32_t now = millis();
    if (now - _lastKickMs > 2000) {
      _lastKickMs = now;
      connect();
    }
  } else {
    _mqtt.loop();
  }

  const uint32_t now = millis();
  if (_mqtt.connected() && now - _lastMqttDebugMs >= 10000UL) {
    const uint32_t age = _lastMsgMs ? (now - _lastMsgMs) : 0;
    webSerial.printf("[MQTT] Loop ok sub=%d lastMsgAge=%u ms lastLen=%u\n",
                     _subscribed ? 1 : 0, (unsigned)age, (unsigned)_lastMsgLen);
    _lastMqttDebugMs = now;
  }

  expireEvents(millis());
}

bool BambuMqttClient::publishRequest(const JsonDocument& doc, bool retain) {
  if (!_ready || !_mqtt.connected()) return false;

  String out;
  serializeJson(doc, out);

  const bool ok = _mqtt.publish(_topicRequest.c_str(), out.c_str(), retain);
  webSerial.printf("[MQTT] Publish request ok=%d len=%u\n", ok ? 1 : 0, (unsigned)out.length());
  return ok;
}

void BambuMqttClient::onReport(ReportCallback cb) {
  _reportCb = cb;
}

const String& BambuMqttClient::topicReport() const { return _topicReport; }
const String& BambuMqttClient::topicRequest() const { return _topicRequest; }
const String& BambuMqttClient::gcodeState() const { return _gcodeState; }
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
  _mqtt.subscribe(_topicReport.c_str(), 0);
  _subscribed = true;
}

void BambuMqttClient::handleMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
  if (!topic || !payload || length == 0) return;
  if (_topicReport.isEmpty()) return;
  if (strcmp(topic, _topicReport.c_str()) != 0) return;

  _lastMsgLen = length;
  _lastMsgMs = millis();

  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  handleReportJson(msg.c_str());
}

void BambuMqttClient::handleReportJson(const char* payload) {
  JsonDocument filter;
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

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    webSerial.printf("[MQTT] JSON parse error: %s\n", err.c_str());
    return;
  }

  if (doc["print"]["gcode_state"].is<const char*>()) {
    _gcodeState = (const char*)doc["print"]["gcode_state"];
  } else if (doc["gcode_state"].is<const char*>()) {
    _gcodeState = (const char*)doc["gcode_state"];
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
    if (p >= 0 && p <= 100) _printProgress = (uint8_t)p;
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
    if (dl >= 0 && dl <= 100) _downloadProgress = (uint8_t)dl;
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
    _bedTemp = bed;
    _bedTarget = bedTarget;
    _bedValid = true;
  }

  float noz = 0.0f;
  float nozTarget = 0.0f;
  bool nozOk = readFloat(doc["print"]["nozzle_temper"], noz) ||
               readFloat(doc["nozzle_temper"], noz);
  bool nozTargetOk = readFloat(doc["print"]["nozzle_target_temper"], nozTarget) ||
                     readFloat(doc["nozzle_target_temper"], nozTarget);

  _nozzleHeating = false;
  JsonVariant extr = doc["device"]["extruder"]["info"];
  if (extr.is<JsonArray>()) {
    for (JsonVariant v : extr.as<JsonArray>()) {
      if (!v.is<JsonObject>()) continue;
      JsonObject e = v.as<JsonObject>();

      int hnow = 0;
      if (readInt(e["hnow"], hnow) && hnow > 0) _nozzleHeating = true;
      int htar = 0;
      if (readInt(e["htar"], htar) && htar > 0) _nozzleHeating = true;

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
    _nozzleTemp = noz;
    _nozzleValid = true;
  }
  if (nozTargetOk) {
    _nozzleTarget = nozTarget;
  }

  parseHmsFromDoc(doc);

  logStatusIfNeeded(millis());

  if (_reportCb) _reportCb(doc);
}

JsonArray BambuMqttClient::findHmsArray(JsonDocument& doc) {
  if (doc["hms"].is<JsonArray>()) return doc["hms"].as<JsonArray>();
  if (doc["print"]["hms"].is<JsonArray>()) return doc["print"]["hms"].as<JsonArray>();
  if (doc["data"]["hms"].is<JsonArray>()) return doc["data"]["hms"].as<JsonArray>();
  return JsonArray();
}

void BambuMqttClient::parseHmsFromDoc(JsonDocument& doc) {
  const uint32_t now = millis();

  JsonArray arr = findHmsArray(doc);
  if (!arr) {
    expireEvents(now);
    return;
  }

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

    upsertEvent(attr, code, now);
  }

  expireEvents(now);
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
