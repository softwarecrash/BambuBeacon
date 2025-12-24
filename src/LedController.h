#pragma once

#include <Arduino.h>
#include <FastLED.h>
#include <ArduinoJson.h>

class Settings;

class LedController {
public:
  LedController();
  ~LedController();

  bool begin(Settings& settings);
  void loop();

  void applySettingsFrom(Settings& settings);
  void ingestBambuReport(JsonObjectConst report, uint32_t nowMs);
  void setMqttConnected(bool connected, uint32_t nowMs);
  void setHmsSeverity(uint8_t sev);
  void setWifiConnected(bool connected);
  void setPrintProgress(uint8_t percent);
  void setDownloadProgress(uint8_t percent);
  void setThermalState(bool heating, bool cooling);
  void setPaused(bool paused);
  void setFinished(bool finished);

  void startSelfTest();

  void setBrightness(uint8_t b);
  void clear(bool showNow = true);

  uint8_t  segments() const { return _segments; }
  uint16_t ledsPerSegment() const { return _perSeg; }
  uint16_t ledCount() const { return _count; }

  void setSegmentColor(uint8_t seg, const CRGB& c, bool showNow = false);
  void setPixel(uint16_t idx, const CRGB& c, bool showNow = false);

  void setGlobalIdle();
  void setNoConnection();

private:
  enum class GlobalState : uint8_t {
    Offline,
    Idle,
    Working,
    Finished,
    Warning,
    Error
  };

  struct RenderState {
    GlobalState global = GlobalState::Offline;
    bool     hasMqtt = false;
    uint32_t lastMqttMs = 0;
    uint8_t  hmsSev = 0; // 0=None, 1=Info, 2=Warning, 3=Error, 4=Fatal
    bool     wifiOk = false;
    uint8_t  printProgress = 255;    // 0-100, 255 = unknown/off
    uint8_t  downloadProgress = 255; // 0-100, 255 = unknown/off
    bool     heating = false;
    bool     cooling = false;
    bool     paused = false;
    bool     finished = false;
  };

  // --- Boot selftest (ampel, segmentweise fill-up) ---
  bool     _bootTestActive;
  uint8_t  _bootSeg;          // current segment (0..segments-1)
  uint16_t _bootPosInSeg;     // next led inside segment
  uint32_t _bootNextMs;       // next step time

  void startBootTest(uint32_t nowMs);
  void tickBootTest(uint32_t nowMs);

  bool alloc(uint16_t count);
  void freeBuf();

  inline uint16_t segStart(uint8_t seg) const { return (uint16_t)seg * _perSeg; }
  inline uint16_t segEnd(uint8_t seg)   const { return segStart(seg) + _perSeg; }

  void markDirty() { _dirty = true; }
  void showIfDirty();

  void tick(uint32_t nowMs);
  void deriveStateFromReport(JsonObjectConst report, uint32_t nowMs);
  void render(uint32_t nowMs);

private:
  CRGB*    _leds;
  uint16_t _perSeg;
  uint8_t  _segments;
  uint16_t _count;
  uint8_t  _brightness;

  bool     _dirty;
  uint32_t _lastTickMs;

  RenderState _st;
};
