#include "GitHubOtaUpdater.h"

#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const uint32_t OTA_TASK_STACK = 8192;
static const uint32_t OTA_HTTP_TIMEOUT_MS = 15000;

GitHubOtaUpdater::GitHubOtaUpdater(const char* owner,
                                   const char* repo,
                                   const char* currentVersion,
                                   const char* buildVariant)
  : _owner(owner ? owner : ""),
    _repo(repo ? repo : ""),
    _currentVersion(currentVersion ? currentVersion : ""),
    _buildVariant(buildVariant ? buildVariant : "") {}

void GitHubOtaUpdater::begin() {
  if (!_lockHandle) {
    _lockHandle = xSemaphoreCreateMutex();
  }
}

void GitHubOtaUpdater::setUpdateActivityCallback(std::function<void(bool active)> cb) {
  _updateActivityCb = cb;
}

bool GitHubOtaUpdater::isBusy() const {
  lock();
  const bool busy = (_state == State::Checking || _state == State::Downloading);
  unlock();
  return busy;
}

bool GitHubOtaUpdater::isUpdateAvailable() const {
  lock();
  const bool available = (_state == State::UpdateAvailable);
  unlock();
  return available;
}

bool GitHubOtaUpdater::takeLastCheckResult(bool* netFail) {
  bool done = false;
  bool nf = false;
  lock();
  if (_lastCheckDone) {
    done = true;
    nf = _lastCheckNetFail;
    _lastCheckDone = false;
    _lastCheckNetFail = false;
  }
  unlock();
  if (netFail) *netFail = nf;
  return done;
}

bool GitHubOtaUpdater::requestCheck() {
  lock();
  const bool busy = (_state == State::Checking || _state == State::Downloading);
  if (busy || _task) {
    unlock();
    return false;
  }
  _bytesDone = 0;
  _bytesTotal = 0;
  _latestVersion = "";
  _assetUrl = "";
  _assetName = "";
  unlock();
  setState(State::Checking, "");

  TaskHandle_t handle = nullptr;
  if (xTaskCreate(checkTask, "bb_ota_check", OTA_TASK_STACK, this, 1, &handle) != pdPASS) {
    setState(State::Error, "Failed to start update check task");
    return false;
  }
  lock();
  _task = handle;
  unlock();
  return true;
}

bool GitHubOtaUpdater::startUpdate() {
  lock();
  const bool okState = (_state == State::UpdateAvailable);
  const bool busy = (_state == State::Checking || _state == State::Downloading);
  if (!okState || busy || _task) {
    unlock();
    return false;
  }
  _bytesDone = 0;
  _bytesTotal = 0;
  unlock();
  setState(State::Downloading, "");

  TaskHandle_t handle = nullptr;
  if (xTaskCreate(downloadTask, "bb_ota_dl", OTA_TASK_STACK, this, 1, &handle) != pdPASS) {
    setState(State::Error, "Failed to start update task");
    return false;
  }
  lock();
  _task = handle;
  unlock();
  return true;
}

String GitHubOtaUpdater::statusJson() const {
  String current;
  String latest;
  String asset;
  String err;
  uint32_t total = 0;
  uint32_t done = 0;
  State state;

  lock();
  current = _currentVersion;
  latest = _latestVersion;
  asset = _assetName;
  err = _lastError;
  total = _bytesTotal;
  done = _bytesDone;
  state = _state;
  unlock();

  const uint8_t pct = (total > 0) ? (uint8_t)((done * 100ULL) / total) : 0;

  JsonDocument doc;
  doc["state"] = (state == State::Idle) ? "idle" :
                 (state == State::Checking) ? "checking" :
                 (state == State::UpToDate) ? "up_to_date" :
                 (state == State::UpdateAvailable) ? "update_available" :
                 (state == State::Downloading) ? "downloading" :
                 (state == State::Success) ? "success" : "error";
  doc["busy"] = (state == State::Checking || state == State::Downloading);
  doc["current"] = current;
  doc["latest"] = latest;
  doc["asset"] = asset;
  doc["bytesTotal"] = total;
  doc["bytesDone"] = done;
  doc["progress"] = pct;
  if (err.length() > 0) {
    doc["error"] = err;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void GitHubOtaUpdater::checkTask(void* param) {
  GitHubOtaUpdater* self = static_cast<GitHubOtaUpdater*>(param);
  self->doCheck();
  self->clearTask();
  vTaskDelete(nullptr);
}

void GitHubOtaUpdater::downloadTask(void* param) {
  GitHubOtaUpdater* self = static_cast<GitHubOtaUpdater*>(param);
  self->doDownload();
  self->clearTask();
  vTaskDelete(nullptr);
}

void GitHubOtaUpdater::doCheck() {
  auto finishCheck = [&](bool netFail) {
    lock();
    _lastCheckDone = true;
    _lastCheckNetFail = netFail;
    unlock();
  };

  if (WiFi.status() != WL_CONNECTED) {
    setState(State::Error, "WiFi not connected");
    finishCheck(true);
    return;
  }

  const String url = "https://api.github.com/repos/" + _owner + "/" + _repo + "/releases/latest";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setUserAgent("BambuBeacon-OTA");

  if (!http.begin(client, url)) {
    setState(State::Error, "Failed to connect to GitHub");
    finishCheck(true);
    return;
  }

  const int code = http.GET();
  if (code != 200) {
    http.end();
    setState(State::Error, "GitHub HTTP " + String(code));
    finishCheck(code <= 0);
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    setState(State::Error, "Release JSON parse error");
    finishCheck(false);
    return;
  }

  const String tag = doc["tag_name"] | doc["name"] | "";
  const String latest = normalizeVersion(tag);
  if (!latest.length()) {
    setState(State::Error, "Missing release version");
    finishCheck(false);
    return;
  }

  lock();
  _latestVersion = latest;
  unlock();

  const int cmp = compareVersions(latest, _currentVersion);
  if (cmp <= 0) {
    setState(State::UpToDate, "");
    finishCheck(false);
    return;
  }

  const JsonArray assets = doc["assets"].as<JsonArray>();
  const String matchToken = "_" + _buildVariant + "_V";
  for (JsonVariantConst v : assets) {
    const String name = v["name"] | "";
    if (!name.length()) continue;
    if (!name.endsWith(".bin.ota")) continue;
    if (name.indexOf(matchToken) < 0) continue;
    const String url = v["browser_download_url"] | "";
    if (!url.length()) continue;

    lock();
    _assetName = name;
    _assetUrl = url;
    unlock();
    setState(State::UpdateAvailable, "");
    finishCheck(false);
    return;
  }

  setState(State::Error, "No matching asset for build");
  finishCheck(false);
}

void GitHubOtaUpdater::doDownload() {
  if (WiFi.status() != WL_CONNECTED) {
    setState(State::Error, "WiFi not connected");
    return;
  }

  struct ScopedActivity {
    std::function<void(bool active)>* cb = nullptr;
    bool active = false;
    explicit ScopedActivity(std::function<void(bool active)>* inCb) : cb(inCb) {}
    void start() {
      if (cb && *cb) {
        (*cb)(true);
        active = true;
      }
    }
    ~ScopedActivity() {
      if (active && cb && *cb) {
        (*cb)(false);
      }
    }
  };

  String url;
  lock();
  url = _assetUrl;
  unlock();
  if (!url.length()) {
    setState(State::Error, "Missing asset URL");
    return;
  }

  ScopedActivity activity(&_updateActivityCb);
  activity.start();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setUserAgent("BambuBeacon-OTA");

  if (!http.begin(client, url)) {
    setState(State::Error, "Failed to connect to asset");
    return;
  }

  const int code = http.GET();
  if (code != 200) {
    http.end();
    setState(State::Error, "Asset HTTP " + String(code));
    return;
  }

  const int total = http.getSize();
  lock();
  _bytesTotal = (total > 0) ? (uint32_t)total : 0;
  _bytesDone = 0;
  unlock();

  if (!Update.begin(total > 0 ? (uint32_t)total : UPDATE_SIZE_UNKNOWN)) {
    http.end();
    setState(State::Error, "Update begin failed");
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = total;

  while (http.connected() && (remaining > 0 || remaining == -1)) {
    const size_t avail = stream->available();
    if (!avail) {
      vTaskDelay(1);
      continue;
    }
    const size_t toRead = (avail > sizeof(buf)) ? sizeof(buf) : avail;
    const size_t readLen = stream->readBytes(buf, toRead);
    if (readLen == 0) {
      vTaskDelay(1);
      continue;
    }
    if (Update.write(buf, readLen) != readLen) {
      Update.abort();
      http.end();
      setState(State::Error, "Update write failed");
      return;
    }
    lock();
    _bytesDone += (uint32_t)readLen;
    unlock();

    if (remaining > 0) {
      remaining -= (int)readLen;
    }
    vTaskDelay(1);
  }

  http.end();

  if (!Update.end(true)) {
    setState(State::Error, "Update finalize failed");
    return;
  }

  lock();
  if (_bytesTotal == 0) {
    _bytesTotal = _bytesDone;
  } else {
    _bytesDone = _bytesTotal;
  }
  unlock();

  setState(State::Success, "");
  scheduleRestart(2500);
}

void GitHubOtaUpdater::scheduleRestart(uint32_t delayMs) {
  esp_timer_handle_t t = nullptr;
  esp_timer_create_args_t args = {};
  args.callback = [](void*) { ESP.restart(); };
  args.arg = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "bb_ota_restart";

  if (esp_timer_create(&args, &t) == ESP_OK && t) {
    esp_timer_start_once(t, (uint64_t)delayMs * 1000ULL);
  } else {
    ESP.restart();
  }
}

void GitHubOtaUpdater::setState(State s, const String& err) {
  lock();
  _state = s;
  _stateTs = millis();
  _lastError = err;
  unlock();
}

void GitHubOtaUpdater::clearTask() {
  lock();
  _task = nullptr;
  unlock();
}

String GitHubOtaUpdater::normalizeVersion(const String& v) {
  String out = v;
  out.trim();
  if (out.length() && (out[0] == 'v' || out[0] == 'V')) {
    out = out.substring(1);
  }
  return out;
}

int GitHubOtaUpdater::compareVersions(const String& a, const String& b) {
  int ai = 0;
  int bi = 0;
  const String av = normalizeVersion(a);
  const String bv = normalizeVersion(b);

  while (ai < (int)av.length() || bi < (int)bv.length()) {
    long na = 0;
    long nb = 0;
    while (ai < (int)av.length() && !isDigit(av[ai])) ai++;
    while (bi < (int)bv.length() && !isDigit(bv[bi])) bi++;
    while (ai < (int)av.length() && isDigit(av[ai])) {
      na = na * 10 + (av[ai] - '0');
      ai++;
    }
    while (bi < (int)bv.length() && isDigit(bv[bi])) {
      nb = nb * 10 + (bv[bi] - '0');
      bi++;
    }
    if (na < nb) return -1;
    if (na > nb) return 1;
  }
  return 0;
}

void GitHubOtaUpdater::lock() const {
  if (_lockHandle) {
    xSemaphoreTake((SemaphoreHandle_t)_lockHandle, portMAX_DELAY);
  }
}

void GitHubOtaUpdater::unlock() const {
  if (_lockHandle) {
    xSemaphoreGive((SemaphoreHandle_t)_lockHandle);
  }
}
