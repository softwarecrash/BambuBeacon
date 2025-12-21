#include "SettingsPrefs.h"
#include "SettingsPrefs.schema.h"

// ---------- SettingsGetter / SettingsSetter ctors ----------

SettingsGetter::SettingsGetter(Settings &outer) : _outer(outer) {}
SettingsSetter::SettingsSetter(Settings &outer) : _outer(outer) {}

// ---------- Settings core ----------

Settings::Settings()
  : get(*this),
    set(*this),
    _initialized(false) {
  // Nothing else here.
}

void Settings::ensureInit() {
  if (_initialized) return;
  begin();
}

void Settings::begin() {
  if (_initialized) return;
  _doc.clear();
  loadFromNvs();
  _initialized = true;
}

void Settings::loadFromNvs() {
  Preferences prefs;

  // Load each item from its NVS namespace.
  // If not existing, default value from schema is used.
  #define LOAD_BOOL(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, true); \
    bool v = prefs.getBool(name, def); \
    prefs.end(); \
    _doc[group][name] = v; \
  }

  #define LOAD_INT32(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, true); \
    int32_t v = prefs.getInt(name, def); \
    prefs.end(); \
    if (v < (minv)) v = (minv); \
    if (v > (maxv)) v = (maxv); \
    _doc[group][name] = v; \
  }

  #define LOAD_UINT16(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, true); \
    uint16_t v = prefs.getUShort(name, (uint16_t)def); \
    prefs.end(); \
    if (v < (uint16_t)(minv)) v = (uint16_t)(minv); \
    if (v > (uint16_t)(maxv)) v = (uint16_t)(maxv); \
    _doc[group][name] = v; \
  }

  #define LOAD_UINT32(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, true); \
    uint32_t v = prefs.getUInt(name, (uint32_t)def); \
    prefs.end(); \
    if (v < (uint32_t)(minv)) v = (uint32_t)(minv); \
    if (v > (uint32_t)(maxv)) v = (uint32_t)(maxv); \
    _doc[group][name] = v; \
  }

  #define LOAD_FLOAT(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, true); \
    float v = prefs.getFloat(name, (float)def); \
    prefs.end(); \
    if (v < (float)(minv)) v = (float)(minv); \
    if (v > (float)(maxv)) v = (float)(maxv); \
    _doc[group][name] = v; \
  }

  #define LOAD_STRING(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, true); \
    String s = prefs.getString(name, def); \
    prefs.end(); \
    _doc[group][name] = s; \
  }

  #define SETTINGS_LOAD(type, group, name, api, def, minv, maxv) \
    LOAD_##type(group, name, api, def, minv, maxv)

  SETTINGS_ITEMS(SETTINGS_LOAD)

  #undef SETTINGS_LOAD
  #undef LOAD_BOOL
  #undef LOAD_INT32
  #undef LOAD_UINT16
  #undef LOAD_UINT32
  #undef LOAD_FLOAT
  #undef LOAD_STRING
}

void Settings::writeToNvs() {
  Preferences prefs;

  #define SAVE_BOOL(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, false); \
    bool v = _doc[group][name] | def; \
    prefs.putBool(name, v); \
    prefs.end(); \
  }

  #define SAVE_INT32(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, false); \
    int32_t v = _doc[group][name] | def; \
    if (v < (minv)) v = (minv); \
    if (v > (maxv)) v = (maxv); \
    prefs.putInt(name, v); \
    prefs.end(); \
  }

  #define SAVE_UINT16(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, false); \
    uint16_t v = (uint16_t)(_doc[group][name] | def); \
    if (v < (uint16_t)(minv)) v = (uint16_t)(minv); \
    if (v > (uint16_t)(maxv)) v = (uint16_t)(maxv); \
    prefs.putUShort(name, v); \
    prefs.end(); \
  }

  #define SAVE_UINT32(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, false); \
    uint32_t v = (uint32_t)(_doc[group][name] | def); \
    if (v < (uint32_t)(minv)) v = (uint32_t)(minv); \
    if (v > (uint32_t)(maxv)) v = (uint32_t)(maxv); \
    prefs.putUInt(name, v); \
    prefs.end(); \
  }

  #define SAVE_FLOAT(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, false); \
    float v = (float)(_doc[group][name] | def); \
    if (v < (float)(minv)) v = (float)(minv); \
    if (v > (float)(maxv)) v = (float)(maxv); \
    prefs.putFloat(name, v); \
    prefs.end(); \
  }

  #define SAVE_STRING(group, name, api, def, minv, maxv) \
  { \
    prefs.begin(group, false); \
    String s = _doc[group][name] | def; \
    prefs.putString(name, s); \
    prefs.end(); \
  }

  #define SETTINGS_SAVE(type, group, name, api, def, minv, maxv) \
    SAVE_##type(group, name, api, def, minv, maxv)

  SETTINGS_ITEMS(SETTINGS_SAVE)

  #undef SETTINGS_SAVE
  #undef SAVE_BOOL
  #undef SAVE_INT32
  #undef SAVE_UINT16
  #undef SAVE_UINT32
  #undef SAVE_FLOAT
  #undef SAVE_STRING
}

bool Settings::save() {
  ensureInit();
  writeToNvs();
  return true; // Preferences has no detailed error reporting
}

String Settings::backup(bool pretty) {
  ensureInit();
  String out;
  if (pretty) {
    serializeJsonPretty(_doc, out);
  } else {
    serializeJson(_doc, out);
  }
  return out;
}

bool Settings::restore(const String &json, bool merge, bool saveAfter) {
  ensureInit();

  JsonDocument tmp;
  DeserializationError err = deserializeJson(tmp, json);
  if (err) {
    return false;
  }

  if (!merge) {
    _doc.clear();
  }

  // Apply only known items, with range checks.
  #define RESTORE_BOOL(group, name, api, def, minv, maxv) \
  { \
    JsonVariant v = tmp[group][name]; \
    if (!v.isNull()) { \
      bool b = v.as<bool>(); \
      _doc[group][name] = b; \
    } \
  }

  #define RESTORE_INT32(group, name, api, def, minv, maxv) \
  { \
    JsonVariant v = tmp[group][name]; \
    if (!v.isNull()) { \
      int32_t val = v.as<int32_t>(); \
      if (val < (minv)) val = (minv); \
      if (val > (maxv)) val = (maxv); \
      _doc[group][name] = val; \
    } \
  }

  #define RESTORE_UINT16(group, name, api, def, minv, maxv) \
  { \
    JsonVariant v = tmp[group][name]; \
    if (!v.isNull()) { \
      uint16_t val = (uint16_t)v.as<uint32_t>(); \
      if (val < (uint16_t)(minv)) val = (uint16_t)(minv); \
      if (val > (uint16_t)(maxv)) val = (uint16_t)(maxv); \
      _doc[group][name] = val; \
    } \
  }

  #define RESTORE_UINT32(group, name, api, def, minv, maxv) \
  { \
    JsonVariant v = tmp[group][name]; \
    if (!v.isNull()) { \
      uint32_t val = v.as<uint32_t>(); \
      if (val < (uint32_t)(minv)) val = (uint32_t)(minv); \
      if (val > (uint32_t)(maxv)) val = (uint32_t)(maxv); \
      _doc[group][name] = val; \
    } \
  }

  #define RESTORE_FLOAT(group, name, api, def, minv, maxv) \
  { \
    JsonVariant v = tmp[group][name]; \
    if (!v.isNull()) { \
      float val = v.as<float>(); \
      if (val < (float)(minv)) val = (float)(minv); \
      if (val > (float)(maxv)) val = (float)(maxv); \
      _doc[group][name] = val; \
    } \
  }

  #define RESTORE_STRING(group, name, api, def, minv, maxv) \
  { \
    JsonVariant v = tmp[group][name]; \
    if (!v.isNull()) { \
      String s = v.as<String>(); \
      _doc[group][name] = s; \
    } \
  }

  #define SETTINGS_RESTORE(type, group, name, api, def, minv, maxv) \
    RESTORE_##type(group, name, api, def, minv, maxv)

  SETTINGS_ITEMS(SETTINGS_RESTORE)

  #undef SETTINGS_RESTORE
  #undef RESTORE_BOOL
  #undef RESTORE_INT32
  #undef RESTORE_UINT16
  #undef RESTORE_UINT32
  #undef RESTORE_FLOAT
  #undef RESTORE_STRING

  if (saveAfter) {
    writeToNvs();
  }
  return true;
}

// ---------- Getter implementations ----------

#define IMPL_GET_BOOL(group, name, api, def, minv, maxv) \
  bool SettingsGetter::api() { \
    _outer.ensureInit(); \
    JsonVariant v = _outer._doc[group][name]; \
    if (v.is<bool>()) return v.as<bool>(); \
    return def; \
  }

#define IMPL_GET_INT32(group, name, api, def, minv, maxv) \
  int32_t SettingsGetter::api() { \
    _outer.ensureInit(); \
    JsonVariant v = _outer._doc[group][name]; \
    int32_t val = v.is<int32_t>() ? v.as<int32_t>() : (int32_t)def; \
    if (val < (minv)) val = (minv); \
    if (val > (maxv)) val = (maxv); \
    return val; \
  }

#define IMPL_GET_UINT16(group, name, api, def, minv, maxv) \
  uint16_t SettingsGetter::api() { \
    _outer.ensureInit(); \
    JsonVariant v = _outer._doc[group][name]; \
    uint16_t val = v.is<uint32_t>() ? (uint16_t)v.as<uint32_t>() : (uint16_t)def; \
    if (val < (uint16_t)(minv)) val = (uint16_t)(minv); \
    if (val > (uint16_t)(maxv)) val = (uint16_t)(maxv); \
    return val; \
  }

#define IMPL_GET_UINT32(group, name, api, def, minv, maxv) \
  uint32_t SettingsGetter::api() { \
    _outer.ensureInit(); \
    JsonVariant v = _outer._doc[group][name]; \
    uint32_t val = v.is<uint32_t>() ? v.as<uint32_t>() : (uint32_t)def; \
    if (val < (uint32_t)(minv)) val = (uint32_t)(minv); \
    if (val > (uint32_t)(maxv)) val = (uint32_t)(maxv); \
    return val; \
  }

#define IMPL_GET_FLOAT(group, name, api, def, minv, maxv) \
  float SettingsGetter::api() { \
    _outer.ensureInit(); \
    JsonVariant v = _outer._doc[group][name]; \
    float val = v.is<float>() ? v.as<float>() : (float)def; \
    if (val < (float)(minv)) val = (float)(minv); \
    if (val > (float)(maxv)) val = (float)(maxv); \
    return val; \
  }

#define IMPL_GET_STRING(group, name, api, def, minv, maxv) \
  const char* SettingsGetter::api() { \
    _outer.ensureInit(); \
    JsonVariant v = _outer._doc[group][name]; \
    if (!v.isNull()) { \
      return v.as<const char*>(); \
    } \
    return def; \
  }

#define SETTINGS_IMPL_GET(type, group, name, api, def, minv, maxv) \
  IMPL_GET_##type(group, name, api, def, minv, maxv)

SETTINGS_ITEMS(SETTINGS_IMPL_GET)

#undef SETTINGS_IMPL_GET
#undef IMPL_GET_BOOL
#undef IMPL_GET_INT32
#undef IMPL_GET_UINT16
#undef IMPL_GET_UINT32
#undef IMPL_GET_FLOAT
#undef IMPL_GET_STRING

// ---------- Setter implementations ----------

#define IMPL_SET_BOOL(group, name, api, def, minv, maxv) \
  void SettingsSetter::api(bool value) { \
    _outer.ensureInit(); \
    _outer._doc[group][name] = value; \
  }

#define IMPL_SET_INT32(group, name, api, def, minv, maxv) \
  void SettingsSetter::api(int32_t value) { \
    _outer.ensureInit(); \
    if (value < (minv)) value = (minv); \
    if (value > (maxv)) value = (maxv); \
    _outer._doc[group][name] = value; \
  }

#define IMPL_SET_UINT16(group, name, api, def, minv, maxv) \
  void SettingsSetter::api(uint16_t value) { \
    _outer.ensureInit(); \
    if (value < (uint16_t)(minv)) value = (uint16_t)(minv); \
    if (value > (uint16_t)(maxv)) value = (uint16_t)(maxv); \
    _outer._doc[group][name] = value; \
  }

#define IMPL_SET_UINT32(group, name, api, def, minv, maxv) \
  void SettingsSetter::api(uint32_t value) { \
    _outer.ensureInit(); \
    if (value < (uint32_t)(minv)) value = (uint32_t)(minv); \
    if (value > (uint32_t)(maxv)) value = (uint32_t)(maxv); \
    _outer._doc[group][name] = value; \
  }

#define IMPL_SET_FLOAT(group, name, api, def, minv, maxv) \
  void SettingsSetter::api(float value) { \
    _outer.ensureInit(); \
    if (value < (float)(minv)) value = (float)(minv); \
    if (value > (float)(maxv)) value = (float)(maxv); \
    _outer._doc[group][name] = value; \
  }

#define IMPL_SET_STRING(group, name, api, def, minv, maxv) \
  void SettingsSetter::api(const String &value) { \
    _outer.ensureInit(); \
    _outer._doc[group][name] = value; \
  }

#define SETTINGS_IMPL_SET(type, group, name, api, def, minv, maxv) \
  IMPL_SET_##type(group, name, api, def, minv, maxv)

SETTINGS_ITEMS(SETTINGS_IMPL_SET)

#undef SETTINGS_IMPL_SET
#undef IMPL_SET_BOOL
#undef IMPL_SET_INT32
#undef IMPL_SET_UINT16
#undef IMPL_SET_UINT32
#undef IMPL_SET_FLOAT
#undef IMPL_SET_STRING
