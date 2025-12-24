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
  _dirty(false),
  _lastTickMs(0),
  _bootTestActive(false),
  _bootSeg(0),
  _bootPosInSeg(0),
  _bootNextMs(0),
  _st() {}

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

  if (_perSeg == 0 || _segments == 0) return false;
  if (!alloc((uint16_t)_perSeg * _segments)) return false;

#ifndef LED_PIN
#error "LED_PIN must be defined via build_flags"
#endif

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(_leds, _count);
  FastLED.setBrightness(_brightness);

  clear(true);

  // OPTIONAL but highly recommended while testing:
  // Serial.printf("LEDperSeg=%u LEDSegments=%u total=%u\n", _perSeg, _segments, _count);

  uint32_t now = millis();
  startBootTest(now);
  _lastTickMs = now;
  return true;
}

void LedController::applySettingsFrom(Settings& settings) {
  uint8_t newBright = (uint8_t)settings.get.LEDBrightness();
  if (newBright != _brightness) {
    _brightness = newBright;
    FastLED.setBrightness(_brightness);
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
  const bool mqttOk = _st.hasMqtt && (uint32_t)(nowMs - _st.lastMqttMs) <= MQTT_STALE_MS;

  if (!mqttOk) {
    setNoConnection();
    return;
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

  if (_st.hmsSev >= 3) {
    if (_segments >= 1 && _perSeg >= 2) {
      const uint16_t pos = (nowMs / 120) % _perSeg;
      const uint16_t opp = (pos + (_perSeg / 2)) % _perSeg;
      const uint16_t base = segStart(0);
      if (base + pos < _count) _leds[base + pos] = CRGB::Red;
      if (base + opp < _count) _leds[base + opp] = CRGB::Red;
    }
  } else if (_st.finished) {
    if (_segments >= 1 && _perSeg >= 1) {
      const uint16_t base = segStart(0);
      const uint32_t lapMs = (uint32_t)_perSeg * 240UL;
      const uint32_t pauseMs = 1200UL;
      const uint32_t phase = (nowMs % (lapMs + pauseMs));
      if (phase < lapMs) {
        const uint16_t pos = phase / 240UL;
        for (uint8_t i = 0; i < 3; i++) {
          const uint16_t idx = (pos + _perSeg - i) % _perSeg;
          uint8_t scale = (i == 0) ? 255 : (i == 1 ? 140 : 60);
          CRGB c = CRGB::Green;
          c.nscale8_video(scale);
          _leds[base + idx] = c;
        }
      }
    }
  } else {
    if (_segments >= 1) setSegmentColor(0, CRGB::Green, false);
  }

  if (_segments >= 2) {
    if (_st.cooling) {
      uint8_t saw = (nowMs / 8) & 0xFF;
      uint8_t level = 255 - saw;
      CRGB c = CRGB(0, 0, 120);
      c.nscale8_video(scale8(level, 180));
      setSegmentColor(1, c, false);
    } else if (_st.heating) {
      uint8_t saw = (nowMs / 8) & 0xFF;
      uint8_t level = saw;
      CRGB c = CRGB(255, 80, 0);
      c.nscale8_video(scale8(level, 200));
      setSegmentColor(1, c, false);
    } else if (_st.paused) {
      setSegmentColor(1, CRGB(255, 150, 0), false);
    } else if (_st.hmsSev == 2) {
      uint8_t pulse = sin8((nowMs / 10) & 0xFF);
      uint8_t level = scale8(pulse, 200) + 30;
      CRGB c = CRGB(255, 150, 0);
      c.nscale8_video(level);
      setSegmentColor(1, c, false);
    } else if (_st.printProgress <= 100) {
      setSegmentColor(1, CRGB::Green, false);
      if (_perSeg > 0) {
        const uint32_t pos16 = ((uint32_t)nowMs * 256UL / 360UL) % ((uint32_t)_perSeg * 256UL);
        const uint16_t idx = (uint16_t)(pos16 >> 8);
        const uint8_t frac = (uint8_t)(pos16 & 0xFF);
        const uint16_t base = segStart(1);
        const uint16_t next = (idx + 1) % _perSeg;

        const uint8_t dimIdx = scale8(255, (uint8_t)(255 - frac));
        const uint8_t dimNext = scale8(255, frac);

        CRGB c0 = CRGB::Green;
        c0.nscale8_video(255 - dimIdx);
        _leds[base + idx] = c0;

        CRGB c1 = CRGB::Green;
        c1.nscale8_video(255 - dimNext);
        _leds[base + next] = c1;
      }
    }
  }

  if (_segments >= 3) {
    if (!_st.wifiOk) {
      uint8_t pulse = sin8((nowMs / 6) & 0xFF);
      uint8_t level = scale8(pulse, 200) + 30;
      CRGB c = CRGB(160, 0, 180);
      c.nscale8_video(level);
      setSegmentColor(2, c, false);
    } else if (_st.downloadProgress <= 100 && _st.downloadProgress < 100) {
      const uint16_t lit = (uint32_t)_perSeg * _st.downloadProgress / 100;
      for (uint16_t i = 0; i < _perSeg && i < lit; i++) {
        _leds[segStart(2) + i] = CRGB::Blue;
      }
    } else if (_st.printProgress <= 100 && _st.printProgress < 100) {
      const uint16_t lit = (uint32_t)_perSeg * _st.printProgress / 100;
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
  if ((uint32_t)(now - _lastTickMs) >= 25) {
    _lastTickMs = now;
    tick(now);
  }
  showIfDirty();
}
