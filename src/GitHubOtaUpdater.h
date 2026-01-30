#pragma once

#include <Arduino.h>
#include <functional>

class GitHubOtaUpdater {
public:
  enum class State : uint8_t {
    Idle,
    Checking,
    UpToDate,
    UpdateAvailable,
    Downloading,
    Success,
    Error
  };

  GitHubOtaUpdater(const char* owner,
                   const char* repo,
                   const char* currentVersion,
                   const char* buildVariant);

  void begin();
  void setUpdateActivityCallback(std::function<void(bool active)> cb);
  bool requestCheck();
  bool startUpdate();
  String statusJson() const;
  bool isBusy() const;
  bool isUpdateAvailable() const;
  bool takeLastCheckResult(bool* netFail);

private:
  static void checkTask(void* param);
  static void downloadTask(void* param);

  void doCheck();
  void doDownload();
  void scheduleRestart(uint32_t delayMs);
  void setState(State s, const String& err = "");
  void clearTask();

  static int compareVersions(const String& a, const String& b);
  static String normalizeVersion(const String& v);

  void lock() const;
  void unlock() const;

  String _owner;
  String _repo;
  String _currentVersion;
  String _buildVariant;

  String _latestVersion;
  String _assetUrl;
  String _assetName;
  String _lastError;
  std::function<void(bool active)> _updateActivityCb;
  bool _lastCheckDone = false;
  bool _lastCheckNetFail = false;

  uint32_t _bytesTotal = 0;
  uint32_t _bytesDone = 0;
  uint32_t _stateTs = 0;
  State _state = State::Idle;

  void* _lockHandle = nullptr;
  void* _task = nullptr;
};
