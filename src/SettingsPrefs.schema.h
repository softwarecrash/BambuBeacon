#pragma once

// Central settings list for the BMSC Preferences-based Settings.
// TYPE,   GROUP,    NAME,          API_NAME,       DEFAULT,       MIN,    MAX
// GROUP = JSON group + NVS namespace
// NAME  = JSON field name + NVS key
// API_NAME = name of getter/setter functions in get./set.
//
// You can freely add new lines here; they are automatically picked up
// by backup/restore, getters/setters, NVS load/save.
//
// Supported TYPE values: BOOL, INT32, UINT16, UINT32, FLOAT, STRING

#define SETTINGS_ITEMS(X) \
  /* ---- Network section ---- */ \
  X(STRING, "network",   "deviceName",         deviceName,      "BMSC2MQTT", 0,    0) \
  X(STRING, "network",   "wifiSsid0",          wifiSsid0,       "",          0,    0) \
  X(STRING, "network",   "wifiBssid0",         wifiBssid0,      "",          0,    0) \
  X(STRING, "network",   "wifiPass0",      wifiPass0,   "",          0,    0) \
  X(STRING, "network",   "wifiSsid1",          wifiSsid1,       "",          0,    0) \
  X(STRING, "network",   "wifiPass1",      wifiPass1,   "",          0,    0) \
  X(STRING, "network",   "staticIP",           staticIP,        "",          0,    0) \
  X(STRING, "network",   "staticGW",           staticGW,        "",          0,    0) \
  X(STRING, "network",   "staticSN",           staticSN,        "",          0,    0) \
  X(STRING, "network",   "staticDNS",          staticDNS,       "",          0,    0) \
  X(STRING, "network",   "webUIuser",          webUIuser,       "",          0,    0) \
  X(STRING, "network",   "webUIPass",      webUIPass,   "",          0,    0) \
  \
  /* ---- MQTT section ---- */ \
  X(STRING, "mqtt",      "host",               mqttHost,        "",         0,      0) \
  X(STRING, "mqtt",      "user",               mqttUser,        "bblp",     0,      0) \
  X(STRING, "mqtt",      "password",           mqttPassword,    "",         0,      0) \
  X(UINT16, "mqtt",      "port",               mqttPort,        8883,       1,  65535) \
  /* ---- Device section ---- */ \
  X(STRING, "device",   "printerUSN",         printerUSN,       "",          0,     0) \
  X(STRING, "device",   "printerIP",          printerIP,        "",          0,     0) \
  X(STRING, "device",   "printerAC",          printerAC,        "",          0,     0) \
  /* End of settings items */
