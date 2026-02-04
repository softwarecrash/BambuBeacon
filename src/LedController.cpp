#include "LedController.h"

#include "main.h"           // LED_PIN via build_flags
#include "SettingsPrefs.h"

static CRGB bootColorForSegment(uint8_t seg) {
  switch (seg) {
    case 0: 
      return CRGB::Red;              // Ring 1 (top) - red
    case 1: 
      return CRGB(255, 90, 0);      // Ring 2 (middle) - traffic amber (reduced green)
    case 2: 
      return CRGB::Green;            // Ring 3 (bottom) - green
    default: 
      return CRGB::White;
  }
}

LedController::LedController()
: _leds(nullptr),
  _perSeg(0),
  _segments(0),
  _count(0),
  _brightness(0),
  _maxCurrentmA(0),
  _reverseOrder(false),
  _idleTimeoutMin(0),
  _lastActiveMs(0),
  _dirty(false),
  _lastTickMs(0),
  _bootTestActive(false),
  _bootSeg(0),
  _bootPosInSeg(0),
  _bootNextMs(0),
  _st(),
  _test(),
  _testMode(false) {}

LedController::~LedController() {
  freeBuf();
}

bool LedController::alloc(uint16_t count) {
  freeBuf();
  if (count == 0) return false;
  _leds = new CRGB[count];
  if (!_leds) return false;
  _count = count;
  return true;
}

void LedController::freeBuf() {
  if (_leds) {
    delete[] _leds;
    _leds = nullptr;
  }
  _count = 0;
}

bool LedController::begin(Settings& settings) {
  _perSeg     = settings.get.LEDperSeg();
  _segments   = (uint8_t)settings.get.LEDSegments();
  _brightness = (uint8_t)settings.get.LEDBrightness();
  _maxCurrentmA = settings.get.LEDMaxCurrentmA();
  _reverseOrder = settings.get.LEDReverseOrder();
  _idleTimeoutMin = settings.get.idleTimeoutMin();

  if (_perSeg == 0 || _segments == 0) return false;
  if (!alloc((uint16_t)_perSeg * _segments)) return false;

#ifndef LED_PIN
#error "LED_PIN must be defined via build_flags"
#endif

  const uint16_t colorOrder = settings.get.LEDColorOrder();
  switch (colorOrder) {
    case 1:
      FastLED.addLeds<WS2812B, LED_PIN, RGB>(_leds, _count);
      break;
    case 2:
      FastLED.addLeds<WS2812B, LED_PIN, BRG>(_leds, _count);
      break;
    case 3:
      FastLED.addLeds<WS2812B, LED_PIN, RBG>(_leds, _count);
      break;
    case 4:
      FastLED.addLeds<WS2812B, LED_PIN, GBR>(_leds, _count);
      break;
    case 5:
      FastLED.addLeds<WS2812B, LED_PIN, BGR>(_leds, _count);
      break;
    case 0:
    default:
      FastLED.addLeds<WS2812B, LED_PIN, GRB>(_leds, _count);
      break;
  }
  FastLED.setBrightness(_brightness);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, _maxCurrentmA);

  clear(true);

  // OPTIONAL but highly recommended while testing:
  // Serial.printf("LEDperSeg=%u LEDSegments=%u total=%u\n", _perSeg, _segments, _count);

  uint32_t now = millis();
  startBootTest(now);
  _lastTickMs = now;
  _lastActiveMs = now;
  return true;
}

void LedController::applySettingsFrom(Settings& settings) {
  uint8_t newBright = (uint8_t)settings.get.LEDBrightness();
  if (newBright != _brightness) {
    _brightness = newBright;
    FastLED.setBrightness(_brightness);
    markDirty();
  }
  uint16_t newMax = settings.get.LEDMaxCurrentmA();
  if (newMax != _maxCurrentmA) {
    _maxCurrentmA = newMax;
    FastLED.setMaxPowerInVoltsAndMilliamps(5, _maxCurrentmA);
  }
  bool newReverse = settings.get.LEDReverseOrder();
  if (newReverse != _reverseOrder) {
    _reverseOrder = newReverse;
    markDirty();
  }
  uint16_t newIdle = settings.get.idleTimeoutMin();
  if (newIdle != _idleTimeoutMin) {
    _idleTimeoutMin = newIdle;
    _lastActiveMs = millis();
    markDirty();
  }
}

void LedController::ingestBambuReport(JsonObjectConst report, uint32_t nowMs) {
  (void)report;
  _st.hasMqtt = true;
  _st.lastMqttMs = nowMs;
  markDirty();
}

void LedController::setMqttConnected(bool connected, uint32_t nowMs) {
  if (connected) {
    _st.hasMqtt = true;
    _st.lastMqttMs = nowMs;
    markDirty();
  }
}

void LedController::setHmsSeverity(uint8_t sev) {
  if (_st.hmsSev != sev) {
    _st.hmsSev = sev;
    markDirty();
  }
}

void LedController::setWifiConnected(bool connected) {
  if (_st.wifiOk != connected) {
    _st.wifiOk = connected;
    markDirty();
  }
}

void LedController::setPrintProgress(uint8_t percent) {
  if (_st.printProgress != percent) {
    _st.printProgress = percent;
    markDirty();
  }
}

void LedController::setDownloadProgress(uint8_t percent) {
  if (_st.downloadProgress != percent) {
    _st.downloadProgress = percent;
    markDirty();
  }
}

void LedController::setThermalState(bool heating, bool cooling) {
  if (_st.heating != heating || _st.cooling != cooling) {
    _st.heating = heating;
    _st.cooling = cooling;
    markDirty();
  }
}

void LedController::setPaused(bool paused) {
  if (_st.paused != paused) {
    _st.paused = paused;
    markDirty();
  }
}

void LedController::setFinished(bool finished) {
  if (_st.finished != finished) {
    _st.finished = finished;
    markDirty();
  }
}

void LedController::startSelfTest() {
  startBootTest(millis());
}

void LedController::setBrightness(uint8_t b) {
  _brightness = b;
  FastLED.setBrightness(_brightness);
  markDirty();
}

void LedController::setTestMode(bool enabled) {
  _testMode = enabled;
  if (_testMode) {
    _test = _st;
    uint32_t now = millis();
    _test.hasMqtt = true;
    _test.lastMqttMs = now;
    _test.wifiOk = true;
    _test.hmsSev = 0;
    _test.printProgress = 255;
    _test.downloadProgress = 255;
    _test.heating = false;
    _test.cooling = false;
    _test.paused = false;
    _test.finished = false;
  }
  markDirty();
}

void LedController::testSetState(const String& state) {
  if (!_testMode) return;
  uint32_t now = millis();

  auto resetFlags = [&]() {
    _test.hmsSev = 0;
    _test.heating = false;
    _test.cooling = false;
    _test.paused = false;
    _test.finished = false;
  };

  if (state == "noconnection") {
    _test.hasMqtt = false;
    markDirty();
    return;
  }

  _test.hasMqtt = true;
  _test.lastMqttMs = now;
  resetFlags();

  if (state == "idle") {
    _test.printProgress = 255;
    _test.downloadProgress = 255;
  } else if (state == "working") {
    if (_test.printProgress > 100) _test.printProgress = 0;
  } else if (state == "finished") {
    _test.finished = true;
  } else if (state == "warning") {
    _test.hmsSev = 2;
  } else if (state == "error") {
    _test.hmsSev = 3;
  } else if (state == "paused") {
    _test.paused = true;
  } else if (state == "heating") {
    _test.heating = true;
  } else if (state == "cooling") {
    _test.cooling = true;
  }

  markDirty();
}

void LedController::testSetWifi(bool ok) {
  if (!_testMode) return;
  _test.wifiOk = ok;
  markDirty();
}

void LedController::testSetMqtt(bool ok) {
  if (!_testMode) return;
  _test.hasMqtt = ok;
  if (ok) _test.lastMqttMs = millis();
  markDirty();
}

void LedController::testSetPrintProgress(uint8_t percent) {
  if (!_testMode) return;
  if (percent > 100) percent = 100;
  _test.printProgress = percent;
  markDirty();
}

void LedController::testSetDownloadProgress(uint8_t percent) {
  if (!_testMode) return;
  if (percent > 100) percent = 100;
  _test.downloadProgress = percent;
  markDirty();
}

void LedController::clear(bool showNow) {
  if (!_leds) return;
  fill_solid(_leds, _count, CRGB::Black);
  _dirty = true;
  if (showNow) FastLED.show();
}

void LedController::setPixel(uint16_t idx, const CRGB& c, bool showNow) {
  if (!_leds || idx >= _count) return;
  _leds[idx] = c;
  markDirty();
  if (showNow) FastLED.show();
}

void LedController::setSegmentColor(uint8_t seg, const CRGB& c, bool showNow) {
  if (!_leds || seg >= _segments) return;
  for (uint16_t i = segStart(seg); i < segEnd(seg); i++)
    _leds[i] = c;
  markDirty();
  if (showNow) FastLED.show();
}

void LedController::showIfDirty() {
  if (!_dirty) return;
  _dirty = false;
  FastLED.show();
}

void LedController::setGlobalIdle() {
  clear(false);
  if (_segments >= 1)
    setSegmentColor(0, CRGB::White, false);
  markDirty();
}

void LedController::setNoConnection() {
  clear(false);
  markDirty();
}

/* ================= Boot Selftest (Ampel, segmentweise) ================= */

void LedController::startBootTest(uint32_t nowMs) {
  if (!_leds) return;

  _bootTestActive = true;
  _bootSeg = 0;
  _bootPosInSeg = 0;
  _bootNextMs = nowMs;

  fill_solid(_leds, _count, CRGB::Black);
  markDirty();
}

void LedController::tickBootTest(uint32_t nowMs) {
  if (!_bootTestActive || !_leds) return;

  const uint32_t STEP_MS = 80; // ruhig
  if ((int32_t)(nowMs - _bootNextMs) < 0) return;

  if (_bootSeg >= _segments) {
    // Done -> stop animation; render loop will take over without forcing white
    _bootTestActive = false;
    return;
  }

  // Turn on next LED inside current segment and keep previous ones on
  if (_bootPosInSeg < _perSeg) {
    uint16_t idx = segStart(_bootSeg) + _bootPosInSeg;
    if (idx < _count) {
      _leds[idx] = bootColorForSegment(_bootSeg);
    }
    _bootPosInSeg++;
    _bootNextMs = nowMs + STEP_MS;
    markDirty();
    return;
  }

  // Segment finished -> next segment
  _bootSeg++;
  _bootPosInSeg = 0;
  _bootNextMs = nowMs + STEP_MS;
}

/* ================= Core ================= */

void LedController::deriveStateFromReport(JsonObjectConst report, uint32_t nowMs) {
  (void)report;
  _st.hasMqtt = true;
  _st.lastMqttMs = nowMs;
}

void LedController::render(uint32_t nowMs) {
  const uint32_t MQTT_STALE_MS = 15000;
  RenderState& st = _testMode ? _test : _st;
  const bool mqttOk = st.hasMqtt && (_testMode || (uint32_t)(nowMs - st.lastMqttMs) <= MQTT_STALE_MS);

  if (!mqttOk) {
    setNoConnection();
    return;
  }

  if (!_testMode && _idleTimeoutMin > 0) {
    const bool active =
      (st.hmsSev >= 2) ||
      st.finished ||
      st.heating ||
      st.cooling ||
      st.paused ||
      (st.printProgress <= 100) ||
      (st.downloadProgress <= 100) ||
      !st.wifiOk;

    if (active) {
      _lastActiveMs = nowMs;
    } else {
      if (_lastActiveMs == 0) _lastActiveMs = nowMs;
      const uint32_t timeoutMs = (uint32_t)_idleTimeoutMin * 60000UL;
      if ((uint32_t)(nowMs - _lastActiveMs) >= timeoutMs) {
        clear(false);
        markDirty();
        return;
      }
    }
  }

  clear(false);

  // LED plan (English, keep synced with behavior):
  // - Ring 0 (top): Green steady when OK/working. Error/Fatal = two red LEDs opposite, rotating (beacon).
  //   Finish = bright 3-LED "comet" that makes a slow lap, then pauses (colorblind-safe).
  // - Ring 1 (middle): Heating = orange-red sawtooth pulse. Cooling after finish = dark blue inverted sawtooth
  //   until bed < 45C. Paused = steady amber. Warning = amber pulse if not heating/cooling/paused.
  //   When printing and no warnings/heating/cooling/paused,
  //   show green ring with one dim LED "gap" rotating slowly (soft fade).
  // - Ring 2 (bottom): Download progress = blue fill, Print progress = green fill, WiFi reconnect = purple blink.
  // Colorblind-friendly: avoid steady green + steady yellow on the same ring; warnings use pulse, errors use motion.

  if (st.hmsSev >= 3) {
    if (_segments >= 1 && _perSeg >= 2) {
      const uint16_t pos = (nowMs / 120) % _perSeg;
      const uint16_t opp = (pos + (_perSeg / 2)) % _perSeg;
      const uint16_t base = segStart(0);
      if (base + pos < _count) _leds[base + pos] = CRGB::Red;
      if (base + opp < _count) _leds[base + opp] = CRGB::Red;
    }
  } else if (st.finished) {
    if (_segments >= 1 && _perSeg >= 1) {
      const uint16_t base = segStart(0);
      const uint32_t lapMs = (uint32_t)_perSeg * 180UL;
      const uint32_t pauseMs = 1400UL;
      const uint32_t phase = (nowMs % (lapMs + pauseMs));
      if (phase < lapMs) {
        const uint32_t pos16 = (phase * 256UL) / 180UL;
        const uint16_t head = (uint16_t)((pos16 >> 8) % _perSeg);
        const uint8_t frac = (uint8_t)(pos16 & 0xFF);
        const uint32_t fadeWindow = 500UL;
        uint8_t fade = 255;
        if (phase < fadeWindow) {
          fade = scale8(255, (uint8_t)min<uint32_t>(255, (phase * 255UL) / fadeWindow));
        } else if (phase > (lapMs - fadeWindow)) {
          const uint32_t tail = lapMs - phase;
          fade = scale8(255, (uint8_t)min<uint32_t>(255, (tail * 255UL) / fadeWindow));
        }

        const uint16_t idx0 = head;
        const uint16_t idx1 = (head + _perSeg - 1) % _perSeg;
        const uint16_t idx2 = (head + _perSeg - 2) % _perSeg;
        const uint16_t idx3 = (head + _perSeg - 3) % _perSeg;

        CRGB c0 = CRGB::Green;
        c0.nscale8_video(scale8((uint8_t)(200 + scale8(frac, 55)), fade));
        _leds[base + idx0] = c0;

        CRGB c1 = CRGB::Green;
        c1.nscale8_video(scale8((uint8_t)(160 - scale8(frac, 60)), fade));
        _leds[base + idx1] = c1;

        CRGB c2 = CRGB::Green;
        c2.nscale8_video(scale8(110, fade));
        _leds[base + idx2] = c2;

        CRGB c3 = CRGB::Green;
        c3.nscale8_video(scale8(70, fade));
        _leds[base + idx3] = c3;
      }
    }
  } else {
    if (_segments >= 1) {
      if (st.paused) {
        uint8_t pulse = sin8((nowMs / 10) & 0xFF);
        uint8_t level = scale8(pulse, 200) + 30;
        CRGB c = CRGB::Green;
        c.nscale8_video(level);
        setSegmentColor(0, c, false);
      } else {
        setSegmentColor(0, CRGB::Green, false);
      }
    }
  }

    if (_segments >= 2) {
    if (st.cooling) {
      uint8_t saw = (nowMs / 8) & 0xFF;
      uint8_t level = 255 - saw;
      CRGB c = CRGB(0, 0, 120);
      c.nscale8_video(scale8(level, 180));
      setSegmentColor(1, c, false);
    } else if (st.heating) {
      uint8_t saw = (nowMs / 8) & 0xFF;
      uint8_t level = saw;
      CRGB c = CRGB(255, 80, 0);
      c.nscale8_video(scale8(level, 200));
      setSegmentColor(1, c, false);
    } else if (st.paused) {
      setSegmentColor(1, CRGB(255, 150, 0), false);
    } else if (st.hmsSev == 2) {
      uint8_t pulse = sin8((nowMs / 10) & 0xFF);
      uint8_t level = scale8(pulse, 200) + 30;
      CRGB c = CRGB(255, 150, 0);
      c.nscale8_video(level);
      setSegmentColor(1, c, false);
    } else if (st.printProgress <= 100) {
      const uint16_t baseIdx = segStart(1);
      const uint8_t baseLevel = 150;
      const uint8_t dipDepth = 220;
      const uint8_t timePhase = (uint8_t)(nowMs / 24);

      for (uint16_t i = 0; i < _perSeg; i++) {
        const uint8_t phase = (uint8_t)(timePhase - (uint8_t)((i * 256U) / _perSeg));
        const uint8_t wave = cos8(phase);
        const uint8_t shaped = qadd8(wave, scale8(wave, 128));
        const uint8_t drop = scale8(shaped, dipDepth);

        int16_t level = (int16_t)baseLevel - (int16_t)drop;
        if (level < 0) level = 0;

        CRGB c = CRGB::Green;
        c.nscale8_video((uint8_t)level);
        _leds[baseIdx + i] = c;
      }
    }
  }

  if (_segments >= 3) {
    if (!st.wifiOk) {
      uint8_t pulse = sin8((nowMs / 6) & 0xFF);
      uint8_t level = scale8(pulse, 200) + 30;
      CRGB c = CRGB(160, 0, 180);
      c.nscale8_video(level);
      setSegmentColor(2, c, false);
    } else if (st.downloadProgress <= 100 && st.downloadProgress < 100) {
      const uint16_t lit = (uint32_t)_perSeg * st.downloadProgress / 100;
      for (uint16_t i = 0; i < _perSeg && i < lit; i++) {
        _leds[segStart(2) + i] = CRGB::Blue;
      }
    } else if (st.printProgress <= 100 && st.printProgress < 100) {
      const uint16_t lit = (uint32_t)_perSeg * st.printProgress / 100;
      for (uint16_t i = 0; i < _perSeg && i < lit; i++) {
        _leds[segStart(2) + i] = CRGB::Green;
      }
    }
  }

  markDirty();
}

void LedController::tick(uint32_t nowMs) {
  if (_bootTestActive) {
    tickBootTest(nowMs);
    return;
  }
  render(nowMs);
}

void LedController::loop() {
  if (!_leds) return;

  uint32_t now = millis();
  if ((uint32_t)(now - _lastTickMs) >= 40) {
    _lastTickMs = now;
    tick(now);
  }
  showIfDirty();
}
