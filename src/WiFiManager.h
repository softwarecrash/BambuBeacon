#pragma once
#include <Arduino.h>

class WiFiManager {
public:
  void begin();
  void loop();

  bool isApMode() const { return _apMode; }

private:
  bool _apMode = false;

  unsigned long _lastTry = 0;
  uint8_t _tries = 0;
  bool _lastFailNoAp = false;

  enum class ConnectPhase : uint8_t { IDLE, SSID0, SSID1 };
  enum class AttemptResult : uint8_t { InProgress, Connected, Failed };
  ConnectPhase _connectPhase = ConnectPhase::IDLE;
  unsigned long _connectStart = 0;

  static const unsigned long kConnectTimeoutMs = 8000UL;
  static const unsigned long kFastFailNoApMs = 2500UL;
  static const unsigned long kRetryIntervalMs = 15000UL;
  static const unsigned long kApRetryIntervalMs = 300000UL;
  static const uint8_t kMaxTriesBeforeAp = 4;

  void startConnectAttempt();
  AttemptResult processConnectAttempt();
  void startAP();
  void stopAP();
};
