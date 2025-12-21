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

  bool connectSTA();
  void startAP();
};
