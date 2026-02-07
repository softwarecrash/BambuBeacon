#include "WebServerHandler.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_timer.h>
#include <Update.h>
#include <pgmspace.h>
#include "SettingsPrefs.h"
#include "WiFiManager.h"
#include "www.h"
#include "WebSerial.h"
#include "bblPrinterDiscovery.h"
#include "BambuMqttClient.h"
#include "LedController.h"
#include "GitHubOtaUpdater.h"
#include "WireGuardVpnManager.h"
#include "VpnSecretStore.h"

extern Settings settings;
extern WiFiManager wifiManager;
extern BBLPrinterDiscovery printerDiscovery;
extern BambuMqttClient bambu;
extern LedController ledsCtrl;
extern GitHubOtaUpdater ota;
extern WireGuardVpnManager wireGuardVpn;
static void scheduleRestart(uint32_t delayMs);

const uint8_t* webserialHtml() {
  return WebSerial_html_gz;
}

size_t webserialHtmlLen() {
  return WebSerial_html_gz_len;
}

// -------------------- Non-blocking WiFi scan cache --------------------
namespace NetScanCache
{
  static bool scanRunning = false;
  static uint32_t cacheTs = 0;
  static String cacheJson;
  static const uint32_t CACHE_MS = 10000;

  static bool cacheValid()
  {
    if (cacheTs == 0) return false;
    return (millis() - cacheTs) < CACHE_MS && cacheJson.length() > 0;
  }

  static void startAsyncScanIfNeeded(bool force)
  {
    if (!force && cacheValid()) return;

    int sc = WiFi.scanComplete();
    if (sc == WIFI_SCAN_RUNNING) {
      scanRunning = true;
      return;
    }

    if (force) {
      if (sc >= 0) {
        WiFi.scanDelete();
      }
      cacheJson = "";
      cacheTs = 0;
      const int rc = WiFi.scanNetworks(true /* async */, true /* show hidden */);
      scanRunning = (rc == WIFI_SCAN_RUNNING);
      return;
    }

    // If results available (>=0), let collectIfFinished() harvest them.
    // Otherwise start a new async scan.
    if (sc < 0) {
      int rc = WiFi.scanNetworks(true /* async */, true /* show hidden */);
      scanRunning = (rc == WIFI_SCAN_RUNNING);
    } else {
      scanRunning = false;
    }
  }

  static void collectIfFinished()
  {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      scanRunning = true;
      return;
    }
    if (n < 0) {
      scanRunning = false;
      return;
    }

    scanRunning = false;

    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();

    for (int i = 0; i < n; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"]  = WiFi.SSID(i);
      o["rssi"]  = WiFi.RSSI(i);
      o["enc"]   = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      o["bssid"] = WiFi.BSSIDstr(i);
    }

    WiFi.scanDelete();

    cacheJson = "";
    serializeJson(doc, cacheJson);
    cacheTs = millis();
  }

  static const String& json()
  {
    return cacheJson;
  }
} // namespace NetScanCache

namespace VpnApi
{
  static const char* kSplitTunnelOnlyError =
      "Full-tunnel configs are not supported. Please use split-tunnel AllowedIPs for the printer network (e.g. 192.168.x.0/24).";
  static const char* kKeyFingerprintMismatch = "Key fingerprint mismatch. Reload page and try again.";

  static bool isLikelyWireGuardKey(const String& value)
  {
    const size_t len = value.length();
    if (len < 40 || len > 80) return false;
    for (size_t i = 0; i < len; i++) {
      const char c = value[i];
      const bool ok =
          (c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') ||
          c == '+' || c == '/' || c == '=' ||
          c == '-' || c == '_';
      if (!ok) return false;
    }
    return true;
  }

  static bool parseIp(const String& value, IPAddress* out)
  {
    if (!out) return false;
    IPAddress ip;
    if (!ip.fromString(value)) return false;
    *out = ip;
    return true;
  }

  static IPAddress maskFromPrefixV4(uint8_t prefix)
  {
    uint32_t mask = 0;
    if (prefix >= 32) {
      mask = 0xFFFFFFFFUL;
    } else if (prefix > 0) {
      mask = 0xFFFFFFFFUL << (32 - prefix);
    }
    return IPAddress(
      (uint8_t)((mask >> 24) & 0xFF),
      (uint8_t)((mask >> 16) & 0xFF),
      (uint8_t)((mask >> 8) & 0xFF),
      (uint8_t)(mask & 0xFF));
  }

  static bool parseIpOrCidr(const String& value, IPAddress* outIp, IPAddress* outMask, bool* hadCidr)
  {
    if (!outIp) return false;
    String tmp = value;
    tmp.trim();
    if (!tmp.length()) return false;

    const int slash = tmp.indexOf('/');
    if (slash < 0) {
      if (hadCidr) *hadCidr = false;
      return parseIp(tmp, outIp);
    }

    String ipPart = tmp.substring(0, slash);
    String prefixPart = tmp.substring(slash + 1);
    ipPart.trim();
    prefixPart.trim();
    if (!ipPart.length() || !prefixPart.length()) return false;

    IPAddress ip;
    if (!ip.fromString(ipPart)) return false;

    for (size_t i = 0; i < prefixPart.length(); i++) {
      const char c = prefixPart[i];
      if (c < '0' || c > '9') return false;
    }

    const long prefix = prefixPart.toInt();
    if (prefix < 0 || prefix > 32) return false;

    *outIp = ip;
    if (outMask) *outMask = maskFromPrefixV4((uint8_t)prefix);
    if (hadCidr) *hadCidr = true;
    return true;
  }

  static IPAddress readIpOrDefault(const char* value, const IPAddress& fallback)
  {
    IPAddress ip;
    if (value && ip.fromString(value)) {
      return ip;
    }
    return fallback;
  }

  static bool isTruthy(const String& value)
  {
    return value == "1" || value == "true" || value == "on";
  }

  static bool isFullTunnelRoute(const IPAddress& allowedIp, const IPAddress& allowedMask)
  {
    (void)allowedIp;
    return allowedMask == IPAddress(0, 0, 0, 0);
  }

  static bool isFullTunnelToken(const String& token)
  {
    String t = token;
    t.trim();
    t.toLowerCase();
    if (t == "::/0") return true;

    const int slash = t.indexOf('/');
    if (slash <= 0 || slash >= (int)t.length() - 1) return false;

    String ipPart = t.substring(0, slash);
    String prefixPart = t.substring(slash + 1);
    ipPart.trim();
    prefixPart.trim();
    if (prefixPart != "0") return false;

    if (ipPart.indexOf(':') >= 0) {
      return true;
    }

    IPAddress ip;
    return ip.fromString(ipPart);
  }

  static bool textContainsFullTunnelEntry(const String& value)
  {
    int start = 0;
    while (start <= (int)value.length()) {
      int comma = value.indexOf(',', start);
      if (comma < 0) comma = value.length();
      String token = value.substring(start, comma);
      token.trim();
      if (token.length() && isFullTunnelToken(token)) return true;
      start = comma + 1;
    }
    return false;
  }

  static bool variantContainsFullTunnelEntry(JsonVariantConst value)
  {
    if (value.isNull()) return false;
    if (value.is<const char*>()) {
      return textContainsFullTunnelEntry(String(value.as<const char*>()));
    }
    if (value.is<String>()) {
      return textContainsFullTunnelEntry(value.as<String>());
    }
    if (value.is<JsonArrayConst>()) {
      JsonArrayConst arr = value.as<JsonArrayConst>();
      for (JsonVariantConst v : arr) {
        if (v.is<const char*>() && textContainsFullTunnelEntry(String(v.as<const char*>()))) {
          return true;
        }
        if (v.is<String>() && textContainsFullTunnelEntry(v.as<String>())) {
          return true;
        }
      }
    }
    return false;
  }

  static VpnConfig loadConfigFromSettings()
  {
    VpnConfig cfg;
    cfg.enabled = settings.get.vpnEnabled();
    cfg.localIp = readIpOrDefault(settings.get.vpnLocalIp(), IPAddress(0, 0, 0, 0));
    cfg.localMask = readIpOrDefault(settings.get.vpnLocalMask(), IPAddress(255, 255, 255, 0));
    cfg.localPort = settings.get.vpnLocalPort();
    cfg.localGateway = readIpOrDefault(settings.get.vpnLocalGateway(), IPAddress(0, 0, 0, 0));
    if (!VpnSecretStore::loadPrivateKey(&cfg.privateKey)) {
      cfg.privateKey = "";
    }
    cfg.endpointHost = settings.get.vpnEndpointHost() ? settings.get.vpnEndpointHost() : "";
    cfg.endpointPublicKey = settings.get.vpnEndpointPubKey() ? settings.get.vpnEndpointPubKey() : "";
    cfg.endpointPort = settings.get.vpnEndpointPort();
    cfg.allowedIp = readIpOrDefault(settings.get.vpnAllowedIp(), IPAddress(0, 0, 0, 0));
    cfg.allowedMask = readIpOrDefault(settings.get.vpnAllowedMask(), IPAddress(0, 0, 0, 0));
    cfg.makeDefault = false;
    if (!VpnSecretStore::loadPresharedKey(&cfg.presharedKey)) {
      cfg.presharedKey = "";
    }
    return cfg;
  }

  static void saveConfigToSettings(const VpnConfig& cfg)
  {
    settings.set.vpnEnabled(cfg.enabled);
    settings.set.vpnLocalIp(cfg.localIp.toString());
    settings.set.vpnLocalMask(cfg.localMask.toString());
    settings.set.vpnLocalPort(cfg.localPort);
    settings.set.vpnLocalGateway(cfg.localGateway.toString());
    settings.set.vpnEndpointHost(cfg.endpointHost);
    settings.set.vpnEndpointPubKey(cfg.endpointPublicKey);
    settings.set.vpnEndpointPort(cfg.endpointPort);
    settings.set.vpnAllowedIp(cfg.allowedIp.toString());
    settings.set.vpnAllowedMask(cfg.allowedMask.toString());
    settings.set.vpnMakeDefault(false);
    settings.save();
  }

  static bool parseBoolField(JsonVariantConst v, bool* out)
  {
    if (!out || v.isNull()) return false;
    if (v.is<bool>()) {
      *out = v.as<bool>();
      return true;
    }
    if (v.is<const char*>()) {
      *out = isTruthy(String(v.as<const char*>()));
      return true;
    }
    if (v.is<int>()) {
      *out = v.as<int>() != 0;
      return true;
    }
    return false;
  }

  static bool parseStringField(JsonVariantConst v, String* out)
  {
    if (!out || v.isNull()) return false;
    if (v.is<const char*>()) {
      *out = String(v.as<const char*>());
      return true;
    }
    if (v.is<String>()) {
      *out = v.as<String>();
      return true;
    }
    return false;
  }

  static bool parsePortField(JsonVariantConst v, uint16_t* out)
  {
    if (!out || v.isNull()) return false;
    long p = -1;
    if (v.is<int>()) {
      p = v.as<int>();
    } else if (v.is<long>()) {
      p = v.as<long>();
    } else if (v.is<const char*>()) {
      p = String(v.as<const char*>()).toInt();
    }
    if (p < 0 || p > 65535) return false;
    *out = (uint16_t)p;
    return true;
  }

  struct SecretUpdateDirective {
    bool hasPrivateKeyNew = false;
    String privateKeyNew;
    bool hasPrivateKeyFp = false;
    String privateKeyFp;
    bool privateKeyClear = false;

    bool hasPresharedKeyNew = false;
    String presharedKeyNew;
    bool hasPresharedKeyFp = false;
    String presharedKeyFp;
    bool presharedKeyClear = false;
  };

  static bool parseAndValidateFromJson(
      JsonObjectConst root,
      const VpnConfig& current,
      VpnConfig* out,
      SecretUpdateDirective* secretUpdate,
      String* errorReason)
  {
    if (!out) return false;

    auto fail = [&](const char* reason) -> bool {
      if (errorReason) *errorReason = reason;
      return false;
    };

    VpnConfig cfg = current;
    String tmp;
    bool boolValue = false;
    uint16_t portValue = 0;
    IPAddress ipValue;
    IPAddress cidrMaskValue;
    bool allowedIpHadCidr = false;
    SecretUpdateDirective secrets;

    if (!root["enabled"].isNull()) {
      if (!parseBoolField(root["enabled"], &boolValue)) return fail("invalid enabled");
      cfg.enabled = boolValue;
    }

    if (!root["local_ip"].isNull()) {
      if (!parseStringField(root["local_ip"], &tmp) || !parseIp(tmp, &ipValue)) return fail("invalid local_ip");
      cfg.localIp = ipValue;
    }
    if (!root["local_mask"].isNull()) {
      if (!parseStringField(root["local_mask"], &tmp) || !parseIp(tmp, &ipValue)) return fail("invalid local_mask");
      cfg.localMask = ipValue;
    }
    if (!root["local_port"].isNull()) {
      if (!parsePortField(root["local_port"], &portValue)) return fail("invalid local_port");
      cfg.localPort = portValue;
    }
    if (!root["local_gateway"].isNull()) {
      if (!parseStringField(root["local_gateway"], &tmp) || !parseIp(tmp, &ipValue)) return fail("invalid local_gateway");
      cfg.localGateway = ipValue;
    }
    if (!root["endpoint_host"].isNull()) {
      if (!parseStringField(root["endpoint_host"], &tmp)) return fail("invalid endpoint_host");
      cfg.endpointHost = tmp;
    }
    if (!root["endpoint_public_key"].isNull()) {
      if (!parseStringField(root["endpoint_public_key"], &tmp)) return fail("invalid endpoint_public_key");
      cfg.endpointPublicKey = tmp;
    }
    if (!root["endpoint_port"].isNull()) {
      if (!parsePortField(root["endpoint_port"], &portValue)) return fail("invalid endpoint_port");
      cfg.endpointPort = portValue;
    }
    if (!root["allowed_ip"].isNull()) {
      if (!parseStringField(root["allowed_ip"], &tmp) ||
          !parseIpOrCidr(tmp, &ipValue, &cidrMaskValue, &allowedIpHadCidr)) {
        return fail("invalid allowed_ip");
      }
      cfg.allowedIp = ipValue;
      if (allowedIpHadCidr) {
        cfg.allowedMask = cidrMaskValue;
      }
    }
    if (!root["allowed_mask"].isNull() && !allowedIpHadCidr) {
      if (!parseStringField(root["allowed_mask"], &tmp) || !parseIp(tmp, &ipValue)) return fail("invalid allowed_mask");
      cfg.allowedMask = ipValue;
    }
    if (!root["make_default"].isNull()) {
      if (!parseBoolField(root["make_default"], &boolValue)) return fail("invalid make_default");
      if (boolValue) return fail(kSplitTunnelOnlyError);
      cfg.makeDefault = false;
    }

    // New secure key semantics.
    if (!root["privateKeyNew"].isNull()) {
      if (!parseStringField(root["privateKeyNew"], &tmp)) return fail("invalid privateKeyNew");
      tmp.trim();
      if (tmp.length()) {
        secrets.hasPrivateKeyNew = true;
        secrets.privateKeyNew = tmp;
      }
    }
    if (!root["privateKeyFp"].isNull()) {
      if (!parseStringField(root["privateKeyFp"], &tmp)) return fail("invalid privateKeyFp");
      tmp.trim();
      if (tmp.length()) {
        secrets.hasPrivateKeyFp = true;
        secrets.privateKeyFp = tmp;
      }
    }
    if (!root["privateKeyClear"].isNull()) {
      if (!parseBoolField(root["privateKeyClear"], &boolValue)) return fail("invalid privateKeyClear");
      secrets.privateKeyClear = boolValue;
    }

    if (!root["presharedKeyNew"].isNull()) {
      if (!parseStringField(root["presharedKeyNew"], &tmp)) return fail("invalid presharedKeyNew");
      tmp.trim();
      if (tmp.length()) {
        secrets.hasPresharedKeyNew = true;
        secrets.presharedKeyNew = tmp;
      }
    }
    if (!root["presharedKeyFp"].isNull()) {
      if (!parseStringField(root["presharedKeyFp"], &tmp)) return fail("invalid presharedKeyFp");
      tmp.trim();
      if (tmp.length()) {
        secrets.hasPresharedKeyFp = true;
        secrets.presharedKeyFp = tmp;
      }
    }
    if (!root["presharedKeyClear"].isNull()) {
      if (!parseBoolField(root["presharedKeyClear"], &boolValue)) return fail("invalid presharedKeyClear");
      secrets.presharedKeyClear = boolValue;
    }

    // Backward compatibility with old payload fields.
    if (!root["private_key"].isNull()) {
      if (!parseStringField(root["private_key"], &tmp)) return fail("invalid private_key");
      tmp.trim();
      if (tmp.length()) {
        secrets.hasPrivateKeyNew = true;
        secrets.privateKeyNew = tmp;
      }
    }
    if (!root["preshared_key"].isNull()) {
      if (!parseStringField(root["preshared_key"], &tmp)) return fail("invalid preshared_key");
      tmp.trim();
      if (tmp.length()) {
        secrets.hasPresharedKeyNew = true;
        secrets.presharedKeyNew = tmp;
      }
    }

    const uint8_t privateModes =
        (secrets.hasPrivateKeyNew ? 1 : 0) +
        (secrets.hasPrivateKeyFp ? 1 : 0) +
        (secrets.privateKeyClear ? 1 : 0);
    if (privateModes > 1) return fail("invalid private key action");

    const uint8_t pskModes =
        (secrets.hasPresharedKeyNew ? 1 : 0) +
        (secrets.hasPresharedKeyFp ? 1 : 0) +
        (secrets.presharedKeyClear ? 1 : 0);
    if (pskModes > 1) return fail("invalid preshared key action");

    if (variantContainsFullTunnelEntry(root["allowed_ips"]) ||
        variantContainsFullTunnelEntry(root["allowed_ips_list"])) {
      return fail(kSplitTunnelOnlyError);
    }

    cfg.endpointHost.trim();
    cfg.endpointPublicKey.trim();
    cfg.makeDefault = false;

    if (isFullTunnelRoute(cfg.allowedIp, cfg.allowedMask)) {
      return fail(kSplitTunnelOnlyError);
    }

    if (cfg.enabled) {
      if (cfg.localIp == IPAddress(0, 0, 0, 0)) return fail("local_ip is required");
      if (cfg.localMask == IPAddress(0, 0, 0, 0)) return fail("local_mask is required");
      if (cfg.localPort == 0) return fail("local_port must be 1..65535");
      if (cfg.endpointPort == 0) return fail("endpoint_port must be 1..65535");
      if (!cfg.endpointHost.length()) return fail("endpoint_host is required");
      if (cfg.endpointHost.length() > 96) return fail("endpoint_host is too long");
      if (!isLikelyWireGuardKey(cfg.endpointPublicKey)) return fail("invalid endpoint_public_key");
    }

    if (secretUpdate) {
      *secretUpdate = secrets;
    }
    *out = cfg;
    return true;
  }

  static bool validateResolvedConfig(const VpnConfig& cfg, String* errorReason)
  {
    auto fail = [&](const char* reason) -> bool {
      if (errorReason) *errorReason = reason;
      return false;
    };

    if (isFullTunnelRoute(cfg.allowedIp, cfg.allowedMask) || cfg.makeDefault) {
      return fail(kSplitTunnelOnlyError);
    }

    if (cfg.enabled) {
      if (cfg.localIp == IPAddress(0, 0, 0, 0)) return fail("local_ip is required");
      if (cfg.localMask == IPAddress(0, 0, 0, 0)) return fail("local_mask is required");
      if (cfg.localPort == 0) return fail("local_port must be 1..65535");
      if (cfg.endpointPort == 0) return fail("endpoint_port must be 1..65535");
      if (!cfg.endpointHost.length()) return fail("endpoint_host is required");
      if (cfg.endpointHost.length() > 96) return fail("endpoint_host is too long");
      if (!isLikelyWireGuardKey(cfg.privateKey)) return fail("invalid private_key");
      if (!isLikelyWireGuardKey(cfg.endpointPublicKey)) return fail("invalid endpoint_public_key");
      if (cfg.presharedKey.length() && !isLikelyWireGuardKey(cfg.presharedKey)) {
        return fail("invalid preshared_key");
      }
    }

    return true;
  }

  static constexpr size_t kMaxUploadBytes = 8192;
  static constexpr size_t kMaxWarnings = 8;
  static constexpr size_t kMaxAllowedCidrs = 24;

  struct WarningList {
    String items[kMaxWarnings];
    size_t count = 0;

    void add(const String& message) {
      if (!message.length()) return;
      for (size_t i = 0; i < count; i++) {
        if (items[i] == message) return;
      }
      if (count < kMaxWarnings) {
        items[count++] = message;
      }
    }
  };

  struct AllowedCidr {
    IPAddress ip;
    uint8_t prefix = 0;
  };

  struct ParsedWgImport {
    bool hasInterfaceAddress = false;
    IPAddress interfaceIp = IPAddress(0, 0, 0, 0);
    uint8_t interfacePrefix = 0;
    bool hasListenPort = false;
    uint16_t listenPort = 0;
    String privateKey;

    uint16_t peerCount = 0;
    String peerPublicKey;
    String peerPresharedKey;
    String endpointHost;
    uint16_t endpointPort = 0;
    String allowedIpsCsv;
    bool hasPersistentKeepalive = false;
    uint16_t persistentKeepalive = 0;
  };

  struct ImportSummary {
    bool hasPrivateKey = false;
    uint16_t peerCount = 0;
    uint16_t allowedIpsCount = 0;
    bool fullTunnel = false;
    bool enabled = false;
  };

  struct PeerTemp {
    bool touched = false;
    String publicKey;
    String presharedKey;
    String endpoint;
    String allowedCsv;
    bool hasPersistentKeepalive = false;
    uint16_t persistentKeepalive = 0;
  };

  static uint32_t prefixToMaskU32(uint8_t prefix) {
    if (prefix == 0) return 0;
    if (prefix >= 32) return 0xFFFFFFFFUL;
    return 0xFFFFFFFFUL << (32 - prefix);
  }

  static IPAddress maskFromPrefix(uint8_t prefix) {
    const uint32_t mask = prefixToMaskU32(prefix);
    return IPAddress(
      (uint8_t)((mask >> 24) & 0xFF),
      (uint8_t)((mask >> 16) & 0xFF),
      (uint8_t)((mask >> 8) & 0xFF),
      (uint8_t)(mask & 0xFF));
  }

  static bool parseUInt16Strict(const String& value, uint16_t* out) {
    if (!out) return false;
    String tmp = value;
    tmp.trim();
    if (!tmp.length()) return false;
    for (size_t i = 0; i < tmp.length(); i++) {
      const char c = tmp[i];
      if (c < '0' || c > '9') return false;
    }
    const long v = tmp.toInt();
    if (v < 0 || v > 65535) return false;
    *out = (uint16_t)v;
    return true;
  }

  static bool parseCidrToken(const String& token, AllowedCidr* out) {
    if (!out) return false;
    String t = token;
    t.trim();
    if (!t.length()) return false;

    const int slash = t.indexOf('/');
    String ipPart = (slash >= 0) ? t.substring(0, slash) : t;
    String prefixPart = (slash >= 0) ? t.substring(slash + 1) : "32";
    ipPart.trim();
    prefixPart.trim();
    if (!ipPart.length()) return false;

    IPAddress ip;
    if (!ip.fromString(ipPart)) return false;

    uint16_t prefixU16 = 32;
    if (!parseUInt16Strict(prefixPart, &prefixU16) || prefixU16 > 32) {
      return false;
    }

    out->ip = ip;
    out->prefix = (uint8_t)prefixU16;
    return true;
  }

  static bool parseEndpointHostPort(const String& endpoint, String* host, uint16_t* port) {
    if (!host || !port) return false;
    String s = endpoint;
    s.trim();
    if (!s.length()) return false;

    String outHost;
    String outPort;
    if (s.startsWith("[")) {
      const int close = s.indexOf(']');
      if (close <= 1 || close + 1 >= (int)s.length() || s[close + 1] != ':') return false;
      outHost = s.substring(1, close);
      outPort = s.substring(close + 2);
    } else {
      const int sep = s.lastIndexOf(':');
      if (sep <= 0 || sep >= (int)s.length() - 1) return false;
      outHost = s.substring(0, sep);
      outPort = s.substring(sep + 1);
    }
    outHost.trim();
    outPort.trim();
    if (!outHost.length()) return false;

    uint16_t p = 0;
    if (!parseUInt16Strict(outPort, &p) || p == 0) return false;
    *host = outHost;
    *port = p;
    return true;
  }

  static bool isRfc1918(const IPAddress& ip) {
    if (ip[0] == 10) return true;
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return true;
    if (ip[0] == 192 && ip[1] == 168) return true;
    return false;
  }

  static String stripInlineComment(const String& line) {
    int pos = -1;
    const int hashPos = line.indexOf('#');
    const int semPos = line.indexOf(';');
    if (hashPos >= 0 && semPos >= 0) pos = (hashPos < semPos) ? hashPos : semPos;
    else if (hashPos >= 0) pos = hashPos;
    else if (semPos >= 0) pos = semPos;
    return (pos >= 0) ? line.substring(0, pos) : line;
  }

  template <typename Func>
  static void splitCsvAndApply(const String& csv, Func cb) {
    int start = 0;
    while (start <= (int)csv.length()) {
      int comma = csv.indexOf(',', start);
      if (comma < 0) comma = csv.length();
      String token = csv.substring(start, comma);
      token.trim();
      if (token.length()) cb(token);
      start = comma + 1;
    }
  }

  class WireGuardConfParser {
  public:
    bool feed(const uint8_t* data, size_t len) {
      if (_hasError) return false;
      _totalBytes += len;
      if (_totalBytes > kMaxUploadBytes) {
        setError("File is too large (max 8 KB).");
        return false;
      }

      for (size_t i = 0; i < len; i++) {
        const char c = (char)data[i];
        if (c == '\r') continue;
        if (c == '\n') {
          processLine(_lineBuffer);
          _lineBuffer = "";
          if (_hasError) return false;
          continue;
        }
        if (_lineBuffer.length() >= 384) {
          setError("A config line is too long.");
          return false;
        }
        _lineBuffer += c;
      }
      return true;
    }

    bool finish() {
      if (_finished) return !_hasError;
      _finished = true;
      if (_lineBuffer.length()) {
        processLine(_lineBuffer);
        _lineBuffer = "";
      }
      finalizeCurrentPeer();
      if (_hasError) return false;

      if (!_hasSelectedPeer && _hasFirstPeer) {
        _selectedPeer = _firstPeer;
        _hasSelectedPeer = true;
      }
      if (!_hasSelectedPeer) {
        setError("No usable [Peer] section found.");
        return false;
      }

      _parsed.peerPublicKey = _selectedPeer.publicKey;
      _parsed.peerPresharedKey = _selectedPeer.presharedKey;
      _parsed.allowedIpsCsv = _selectedPeer.allowedCsv;
      _parsed.hasPersistentKeepalive = _selectedPeer.hasPersistentKeepalive;
      _parsed.persistentKeepalive = _selectedPeer.persistentKeepalive;

      if (!parseEndpointHostPort(_selectedPeer.endpoint, &_parsed.endpointHost, &_parsed.endpointPort)) {
        setError("Invalid peer Endpoint.");
        return false;
      }
      return true;
    }

    bool hasError() const { return _hasError; }
    const String& error() const { return _error; }
    const ParsedWgImport& parsed() const { return _parsed; }

  private:
    enum class Section : uint8_t { None, Interface, Peer };

    void setError(const char* message) {
      if (_hasError) return;
      _hasError = true;
      _error = message;
    }

    void parseAddressValue(const String& value) {
      splitCsvAndApply(value, [&](const String& token) {
        if (_parsed.hasInterfaceAddress) return;
        AllowedCidr cidr;
        if (!parseCidrToken(token, &cidr)) return;
        _parsed.interfaceIp = cidr.ip;
        _parsed.interfacePrefix = cidr.prefix;
        _parsed.hasInterfaceAddress = true;
      });
    }

    void processLine(const String& rawLine) {
      if (_hasError) return;
      String line = stripInlineComment(rawLine);
      line.trim();
      if (!line.length()) return;

      if (line.startsWith("[") && line.endsWith("]")) {
        finalizeCurrentPeer();
        String sectionName = line.substring(1, line.length() - 1);
        sectionName.trim();
        sectionName.toLowerCase();
        if (sectionName == "interface") _section = Section::Interface;
        else if (sectionName == "peer") _section = Section::Peer;
        else _section = Section::None;
        return;
      }

      const int eq = line.indexOf('=');
      if (eq < 0) return;
      String key = line.substring(0, eq);
      String value = line.substring(eq + 1);
      key.trim();
      value.trim();
      key.toLowerCase();

      if (_section == Section::Interface) {
        if (key == "privatekey") {
          _parsed.privateKey = value;
        } else if (key == "address") {
          parseAddressValue(value);
        } else if (key == "listenport") {
          uint16_t port = 0;
          if (parseUInt16Strict(value, &port) && port > 0) {
            _parsed.hasListenPort = true;
            _parsed.listenPort = port;
          }
        }
      } else if (_section == Section::Peer) {
        _currentPeer.touched = true;
        if (key == "publickey") {
          _currentPeer.publicKey = value;
        } else if (key == "presharedkey") {
          _currentPeer.presharedKey = value;
        } else if (key == "endpoint") {
          _currentPeer.endpoint = value;
        } else if (key == "allowedips") {
          if (_currentPeer.allowedCsv.length()) _currentPeer.allowedCsv += ",";
          _currentPeer.allowedCsv += value;
        } else if (key == "persistentkeepalive") {
          uint16_t keep = 0;
          if (parseUInt16Strict(value, &keep)) {
            _currentPeer.hasPersistentKeepalive = true;
            _currentPeer.persistentKeepalive = keep;
          }
        }
      }
    }

    void finalizeCurrentPeer() {
      if (!_currentPeer.touched &&
          !_currentPeer.publicKey.length() &&
          !_currentPeer.endpoint.length() &&
          !_currentPeer.allowedCsv.length() &&
          !_currentPeer.presharedKey.length()) {
        _currentPeer = PeerTemp{};
        return;
      }

      _parsed.peerCount++;
      if (!_hasFirstPeer) {
        _firstPeer = _currentPeer;
        _hasFirstPeer = true;
      }
      if (!_hasSelectedPeer &&
          _currentPeer.publicKey.length() &&
          _currentPeer.endpoint.length()) {
        _selectedPeer = _currentPeer;
        _hasSelectedPeer = true;
      }
      _currentPeer = PeerTemp{};
    }

    bool _finished = false;
    bool _hasError = false;
    size_t _totalBytes = 0;
    String _error;
    String _lineBuffer;
    Section _section = Section::None;
    ParsedWgImport _parsed;
    PeerTemp _currentPeer;
    PeerTemp _firstPeer;
    PeerTemp _selectedPeer;
    bool _hasFirstPeer = false;
    bool _hasSelectedPeer = false;
  };

  static bool parseAllowedCidrs(
      const String& csv,
      AllowedCidr* out,
      size_t outCap,
      size_t* storedCount,
      size_t* totalCount,
      size_t* fullTunnelCount)
  {
    if (!storedCount || !totalCount || !fullTunnelCount) return false;
    *storedCount = 0;
    *totalCount = 0;
    *fullTunnelCount = 0;

    splitCsvAndApply(csv, [&](const String& token) {
      if (isFullTunnelToken(token)) {
        (*fullTunnelCount)++;
        return;
      }
      AllowedCidr cidr;
      if (!parseCidrToken(token, &cidr)) return;
      (*totalCount)++;
      if (*storedCount < outCap) {
        out[*storedCount] = cidr;
        (*storedCount)++;
      }
    });
    return true;
  }

  static bool buildConfigFromImport(
      const ParsedWgImport& parsed,
      const VpnConfig& current,
      VpnConfig* outConfig,
      WarningList* warnings,
      ImportSummary* summary,
      String* error)
  {
    auto fail = [&](const char* reason) -> bool {
      if (error) *error = reason;
      return false;
    };

    if (!outConfig || !warnings || !summary) return fail("Internal import error.");
    if (!parsed.hasInterfaceAddress) return fail("Missing [Interface] Address with IPv4 CIDR.");
    if (parsed.peerCount == 0) return fail("No [Peer] section found.");
    if (!parsed.peerPublicKey.length()) return fail("Missing [Peer] PublicKey.");
    if (!parsed.endpointHost.length() || parsed.endpointPort == 0) return fail("Missing or invalid [Peer] Endpoint.");

    VpnConfig cfg = current;
    cfg.localIp = parsed.interfaceIp;
    cfg.localMask = maskFromPrefix(parsed.interfacePrefix);
    cfg.localGateway = IPAddress(0, 0, 0, 0);
    if (parsed.hasListenPort && parsed.listenPort > 0) cfg.localPort = parsed.listenPort;
    if (cfg.localPort == 0) cfg.localPort = 33333;
    cfg.privateKey = parsed.privateKey;
    cfg.endpointHost = parsed.endpointHost;
    cfg.endpointPublicKey = parsed.peerPublicKey;
    cfg.endpointPort = parsed.endpointPort;
    cfg.presharedKey = parsed.peerPresharedKey;
    cfg.makeDefault = false;
    if (parsed.hasPersistentKeepalive && parsed.persistentKeepalive != 25) {
      warnings->add("PersistentKeepalive from config is ignored; device uses 25 seconds.");
    }

    AllowedCidr allowed[kMaxAllowedCidrs];
    size_t allowedStored = 0;
    size_t allowedTotal = 0;
    size_t fullTunnelDropped = 0;
    parseAllowedCidrs(parsed.allowedIpsCsv, allowed, kMaxAllowedCidrs, &allowedStored, &allowedTotal, &fullTunnelDropped);
    if (allowedTotal == 0) return fail(kSplitTunnelOnlyError);
    if (allowedTotal > allowedStored) {
      warnings->add("Too many Allowed IPs detected; only the first entries were parsed.");
    }

    summary->hasPrivateKey = parsed.privateKey.length() > 0;
    summary->peerCount = parsed.peerCount;
    summary->allowedIpsCount = (uint16_t)allowedTotal;
    summary->fullTunnel = fullTunnelDropped > 0;
    if (fullTunnelDropped > 0) {
      warnings->add("Full-tunnel entry 0.0.0.0/0 was ignored to keep local access safe.");
    }

    if (parsed.peerCount > 1) {
      warnings->add("Multiple peers detected; only the first compatible peer is used.");
    }

    int firstRfc = -1;
    int rfcCount = 0;

    for (size_t i = 0; i < allowedStored; i++) {
      const AllowedCidr& a = allowed[i];
      if (isRfc1918(a.ip)) {
        if (firstRfc < 0) firstRfc = (int)i;
        rfcCount++;
      }
    }

    if (firstRfc < 0) {
      return fail(kSplitTunnelOnlyError);
    }

    cfg.allowedIp = allowed[firstRfc].ip;
    cfg.allowedMask = maskFromPrefix(allowed[firstRfc].prefix);
    cfg.makeDefault = false;

    if (allowedTotal > 1) {
      warnings->add("Multiple Allowed IPs detected; only the first subnet is used.");
    }
    if ((size_t)rfcCount < allowedTotal) {
      warnings->add("Non-private Allowed IP entries were ignored.");
    }

    const bool hasRequired =
        cfg.localIp != IPAddress(0, 0, 0, 0) &&
        cfg.localMask != IPAddress(0, 0, 0, 0) &&
        cfg.localPort > 0 &&
        cfg.endpointHost.length() &&
        cfg.endpointPort > 0 &&
        isLikelyWireGuardKey(cfg.privateKey) &&
        isLikelyWireGuardKey(cfg.endpointPublicKey) &&
        (!cfg.presharedKey.length() || isLikelyWireGuardKey(cfg.presharedKey));

    cfg.enabled = hasRequired;
    if (!hasRequired) {
      warnings->add("Required WireGuard fields are incomplete; VPN was not enabled.");
    }
    summary->enabled = hasRequired;
    *outConfig = cfg;
    return true;
  }

  static bool isAllowedImportFilename(const String& filename) {
    if (!filename.length()) return true;
    String lower = filename;
    lower.toLowerCase();
    return lower.endsWith(".conf") || lower.endsWith(".txt");
  }
} // namespace VpnApi

struct VpnImportUploadState {
  VpnApi::WireGuardConfParser parser;
  bool fileSeen = false;
  String error;
};

// -------------------- Restart scheduling (no delay in handlers) --------------------
static void bb_restart_cb(void* arg)
{
  (void)arg;
  ESP.restart();
}

static void scheduleRestart(uint32_t delayMs)
{
  esp_timer_handle_t t = nullptr;
  esp_timer_create_args_t args = {};
  args.callback = &bb_restart_cb;
  args.arg = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "bb_restart";

  if (esp_timer_create(&args, &t) == ESP_OK && t) {
    esp_timer_start_once(t, (uint64_t)delayMs * 1000ULL);
  } else {
    ESP.restart();
  }
}

WebServerHandler::WebServerHandler(AsyncWebServer& s) : server(s) {}

bool WebServerHandler::isAuthorized(AsyncWebServerRequest* req) {
  // If user is empty => no auth
  if (!settings.get.webUIuser() || !*settings.get.webUIuser()) return true;
  return req->authenticate(settings.get.webUIuser(), settings.get.webUIPass());
}

void WebServerHandler::sendGz(AsyncWebServerRequest* req, const uint8_t* data, size_t len, const char* mime) {
  AsyncWebServerResponse* r = req->beginResponse(200, mime, data, len);
  r->addHeader("Content-Encoding", "gzip");
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

void WebServerHandler::sendGzChunked(AsyncWebServerRequest* req, const uint8_t* data, size_t len, const char* mime) {
  AsyncWebServerResponse* r = req->beginChunkedResponse(mime, [data, len](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
    if (index >= len) return 0;
    size_t n = len - index;
    if (n > maxLen) n = maxLen;
    memcpy_P(buffer, data + index, n);
    return n;
  });
  r->addHeader("Content-Encoding", "gzip");
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

void WebServerHandler::handleNetlist(AsyncWebServerRequest* req) {
  // Never run synchronous WiFi scans inside AsyncTCP handlers.
  // Trigger async scan and return cached results immediately.
  const bool force = req->hasParam("force");
  NetScanCache::startAsyncScanIfNeeded(force);
  NetScanCache::collectIfFinished();

  if (NetScanCache::cacheValid()) {
    req->send(200, "application/json", NetScanCache::json());
    return;
  }

  // No cache yet -> respond with empty list (HTML remains unchanged).
  req->send(200, "application/json", "{\"networks\":[]}");
}

void WebServerHandler::handleSubmitConfig(AsyncWebServerRequest* req) {
  auto getP = [&](const char* name) -> String {
    if (!req->hasParam(name, true)) return "";
    return req->getParam(name, true)->value();
  };

  settings.set.deviceName(getP("devicename"));

  settings.set.wifiSsid0(getP("ssid0"));
  settings.set.wifiPass0(getP("password0"));
  settings.set.wifiBssid0(getP("bssid0"));
  const String bssidLock = getP("bssidLock");
  if (bssidLock.length()) {
    const bool lock = (bssidLock == "1" || bssidLock == "true" || bssidLock == "on");
    settings.set.wifiBssidLock(lock);
  }

  // FIX: ssid1 must go to wifiSsid1 (was wrongly written to wifiSsid0)
  settings.set.wifiSsid1(getP("ssid1"));
  settings.set.wifiPass1(getP("password1"));

  settings.set.staticIP(getP("ip"));
  settings.set.staticSN(getP("subnet"));
  settings.set.staticGW(getP("gateway"));
  settings.set.staticDNS(getP("dns"));

  settings.set.webUIuser(getP("webUser"));
  settings.set.webUIPass(getP("webPass"));

  settings.save();

  req->send(200, "application/json", "{\"success\":true}");

  // Do not block inside async request handlers
  scheduleRestart(600);
}

void WebServerHandler::handlePrinterDiscovery(AsyncWebServerRequest* req) {
  if (req->hasParam("rescan")) {
    printerDiscovery.forceRescan(0);
  }

  JsonDocument doc;
  JsonArray arr = doc["printers"].to<JsonArray>();

  const int n = printerDiscovery.knownCount();
  const BBLPrinter* printers = printerDiscovery.knownPrinters();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["usn"] = printers[i].usn;
    o["ip"] = printers[i].ip.toString();
  }

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

void WebServerHandler::handleSubmitPrinterConfig(AsyncWebServerRequest* req) {
  auto getP = [&](const char* name) -> String {
    if (!req->hasParam(name, true)) return "";
    return req->getParam(name, true)->value();
  };

  const String oldIp = settings.get.printerIP() ? settings.get.printerIP() : "";
  const String oldUsn = settings.get.printerUSN() ? settings.get.printerUSN() : "";

  const uint16_t oldSeg = settings.get.LEDSegments();
  const uint16_t oldPer = settings.get.LEDperSeg();
  const uint16_t oldColorOrder = settings.get.LEDColorOrder();

  const String newIp = getP("printerip");
  const String newUsn = getP("printerusn");
  settings.set.printerIP(newIp);
  settings.set.printerUSN(newUsn);
  settings.set.printerAC(getP("printerac"));

  if (newIp != oldIp || newUsn != oldUsn) {
    settings.set.printerCert("");
  }

  if (req->hasParam("ledsegments", true)) {
    long v = getP("ledsegments").toInt();
    if (v < 2) v = 2;
    if (v > 3) v = 3;
    settings.set.LEDSegments((uint16_t)v);
  }

  if (req->hasParam("ledperseg", true)) {
    long v = getP("ledperseg").toInt();
    if (v < 1) v = 1;
    if (v > 64) v = 64;
    settings.set.LEDperSeg((uint16_t)v);
  }

  if (req->hasParam("ledmaxcurrent", true)) {
    long v = getP("ledmaxcurrent").toInt();
    if (v < 100) v = 100;
    if (v > 5000) v = 5000;
    settings.set.LEDMaxCurrentmA((uint16_t)v);
  }

  if (req->hasParam("ledreverse", true)) {
    const String v = getP("ledreverse");
    const bool enabled = (v == "1" || v == "true" || v == "on");
    settings.set.LEDReverseOrder(enabled);
  }

  if (req->hasParam("ledcolororder", true)) {
    long v = getP("ledcolororder").toInt();
    if (v < 0) v = 0;
    if (v > 5) v = 5;
    settings.set.LEDColorOrder((uint16_t)v);
  }

  if (req->hasParam("idletimeout", true)) {
    long v = getP("idletimeout").toInt();
    if (v < 0) v = 0;
    if (v > 240) v = 240;
    settings.set.idleTimeoutMin((uint16_t)v);
  }

  settings.save();
  ledsCtrl.applySettingsFrom(settings);

  bambu.reloadFromSettings();
  if (WiFi.status() == WL_CONNECTED) bambu.connect();

  req->send(200, "application/json", "{\"success\":true}");

  if (settings.get.LEDSegments() != oldSeg ||
      settings.get.LEDperSeg() != oldPer ||
      settings.get.LEDColorOrder() != oldColorOrder) {
    scheduleRestart(600);
  }
}

void WebServerHandler::handleLedTestCmd(AsyncWebServerRequest* req) {
  auto getP = [&](const char* name) -> String {
    if (!req->hasParam(name, true)) return "";
    return req->getParam(name, true)->value();
  };

  const String action = getP("action");
  const String value = getP("value");

  if (action == "mode") {
    const bool enable = (value == "on" || value == "1" || value == "true");
    ledsCtrl.setTestMode(enable);
    req->send(200, "application/json", "{\"success\":true}");
    return;
  }

  if (!ledsCtrl.testMode()) {
    req->send(200, "application/json", "{\"success\":false,\"reason\":\"testmode\"}");
    return;
  }

  if (action == "state") {
    ledsCtrl.testSetState(value);
  } else if (action == "wifi") {
    ledsCtrl.testSetWifi(value != "0");
  } else if (action == "mqtt") {
    ledsCtrl.testSetMqtt(value != "0");
  } else if (action == "print") {
    long v = value.toInt();
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    ledsCtrl.testSetPrintProgress((uint8_t)v);
  } else if (action == "download") {
    long v = value.toInt();
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    ledsCtrl.testSetDownloadProgress((uint8_t)v);
  } else if (action == "update") {
    const bool available = (value == "1" || value == "true" || value == "on");
    ledsCtrl.testSetUpdateAvailable(available);
  } else {
    req->send(400, "application/json", "{\"success\":false}");
    return;
  }

  req->send(200, "application/json", "{\"success\":true}");
}

void WebServerHandler::handleGetVpnApi(AsyncWebServerRequest* req) {
  (void)req;
  const VpnConfig cfg = VpnApi::loadConfigFromSettings();
  const VpnSecretStore::KeyMeta privateMeta = VpnSecretStore::privateKeyMeta();
  const VpnSecretStore::KeyMeta pskMeta = VpnSecretStore::presharedKeyMeta();

  JsonDocument doc;
  JsonObject config = doc["config"].to<JsonObject>();
  config["enabled"] = cfg.enabled;
  config["local_ip"] = cfg.localIp.toString();
  config["local_mask"] = cfg.localMask.toString();
  config["local_port"] = cfg.localPort;
  config["local_gateway"] = cfg.localGateway.toString();
  config["endpoint_host"] = cfg.endpointHost;
  config["endpoint_public_key"] = cfg.endpointPublicKey;
  config["endpoint_port"] = cfg.endpointPort;
  config["allowed_ip"] = cfg.allowedIp.toString();
  config["allowed_mask"] = cfg.allowedMask.toString();
  config["make_default"] = false;
  config["hasPrivateKey"] = privateMeta.has;
  config["privateKeyFp"] = privateMeta.fingerprint;
  config["privateKeyFpDisplay"] = privateMeta.displayFingerprint;
  config["hasPresharedKey"] = pskMeta.has;
  config["presharedKeyFp"] = pskMeta.fingerprint;
  config["presharedKeyFpDisplay"] = pskMeta.displayFingerprint;

  JsonObject status = doc["status"].to<JsonObject>();
  status["connected"] = wireGuardVpn.isConnected();
  status["statusText"] = wireGuardVpn.statusText();
  status["lastHandshakeSeconds"] = wireGuardVpn.lastHandshakeSeconds();

  String out;
  serializeJson(doc, out);
  AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

void WebServerHandler::handleSetVpnApi(AsyncWebServerRequest* req, const String& body) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err || !doc.is<JsonObject>()) {
    req->send(400, "application/json", "{\"success\":false,\"reason\":\"invalid_json\"}");
    return;
  }

  const VpnConfig current = VpnApi::loadConfigFromSettings();
  VpnConfig updated = current;
  VpnApi::SecretUpdateDirective secretUpdate;
  String reason;
  if (!VpnApi::parseAndValidateFromJson(doc.as<JsonObjectConst>(), current, &updated, &secretUpdate, &reason)) {
    JsonDocument outDoc;
    outDoc["success"] = false;
    outDoc["reason"] = reason;
    String out;
    serializeJson(outDoc, out);
    req->send(400, "application/json", out);
    return;
  }

  // Resolve secret key operations without ever returning key material.
  {
    VpnSecretStore::KeyMeta privateMeta = VpnSecretStore::privateKeyMeta();
    VpnSecretStore::KeyMeta pskMeta = VpnSecretStore::presharedKeyMeta();

    String privateKey = current.privateKey;
    if (secretUpdate.hasPrivateKeyNew) {
      privateKey = secretUpdate.privateKeyNew;
      privateKey.trim();
      if (!VpnApi::isLikelyWireGuardKey(privateKey)) {
        reason = "invalid private_key";
      }
    } else if (secretUpdate.privateKeyClear) {
      privateKey = "";
    } else if (secretUpdate.hasPrivateKeyFp) {
      if (!privateMeta.has || !VpnSecretStore::fingerprintsMatch(secretUpdate.privateKeyFp, privateMeta.fingerprint)) {
        reason = VpnApi::kKeyFingerprintMismatch;
      }
    }

    String psk = current.presharedKey;
    if (!reason.length()) {
      if (secretUpdate.hasPresharedKeyNew) {
        psk = secretUpdate.presharedKeyNew;
        psk.trim();
        if (psk.length() && !VpnApi::isLikelyWireGuardKey(psk)) {
          reason = "invalid preshared_key";
        }
      } else if (secretUpdate.presharedKeyClear) {
        psk = "";
      } else if (secretUpdate.hasPresharedKeyFp) {
        if (!pskMeta.has || !VpnSecretStore::fingerprintsMatch(secretUpdate.presharedKeyFp, pskMeta.fingerprint)) {
          reason = VpnApi::kKeyFingerprintMismatch;
        }
      }
    }

    if (reason.length()) {
      JsonDocument outDoc;
      outDoc["success"] = false;
      outDoc["reason"] = reason;
      String out;
      serializeJson(outDoc, out);
      req->send(400, "application/json", out);
      return;
    }

    updated.privateKey = privateKey;
    updated.presharedKey = psk;
  }

  if (!VpnApi::validateResolvedConfig(updated, &reason)) {
    JsonDocument outDoc;
    outDoc["success"] = false;
    outDoc["reason"] = reason;
    String out;
    serializeJson(outDoc, out);
    req->send(400, "application/json", out);
    return;
  }

  VpnApi::saveConfigToSettings(updated);
  if (updated.privateKey.length()) {
    VpnSecretStore::setPrivateKey(updated.privateKey);
  } else {
    VpnSecretStore::clearPrivateKey();
  }
  if (updated.presharedKey.length()) {
    VpnSecretStore::setPresharedKey(updated.presharedKey);
  } else {
    VpnSecretStore::clearPresharedKey();
  }

  bool applied = true;
  if (updated.enabled) {
    applied = wireGuardVpn.begin(updated);
  } else {
    wireGuardVpn.end();
  }

  JsonDocument outDoc;
  outDoc["success"] = true;
  outDoc["applied"] = applied;
  JsonObject status = outDoc["status"].to<JsonObject>();
  status["connected"] = wireGuardVpn.isConnected();
  status["statusText"] = wireGuardVpn.statusText();
  status["lastHandshakeSeconds"] = wireGuardVpn.lastHandshakeSeconds();

  String out;
  serializeJson(outDoc, out);
  req->send(200, "application/json", out);
}

void WebServerHandler::begin() {
  auto captivePortalResponse = [&](AsyncWebServerRequest* req) {
    if (wifiManager.isApMode()) {
      sendGz(req, WiFiSetup_html_gz, WiFiSetup_html_gz_len, WiFiSetup_html_gz_mime);
      return;
    }
    req->send(404, "text/plain", "Not found");
  };

  // Root
  server.on("/", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (wifiManager.isApMode()) {
      req->redirect("/wifisetup");
      return;
    }
    if (!isAuthorized(req)) return req->requestAuthentication();
    sendGz(req, Status_html_gz, Status_html_gz_len, Status_html_gz_mime);
  });

  // WiFi setup should always be reachable in AP mode without login
  server.on("/wifisetup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    // Start scan aggressively when entering setup page
    NetScanCache::startAsyncScanIfNeeded(true);
    sendGz(req, WiFiSetup_html_gz, WiFiSetup_html_gz_len, WiFiSetup_html_gz_mime);
  });

  // Captive portal detection endpoints (Android/iOS/Windows)
  server.on("/generate_204", HTTP_GET, captivePortalResponse);
  server.on("/gen_204", HTTP_GET, captivePortalResponse);
  server.on("/hotspot-detect.html", HTTP_GET, captivePortalResponse);
  server.on("/library/test/success.html", HTTP_GET, captivePortalResponse);
  server.on("/ncsi.txt", HTTP_GET, captivePortalResponse);
  server.on("/connecttest.txt", HTTP_GET, captivePortalResponse);
  server.on("/fwlink", HTTP_GET, captivePortalResponse);

  server.on("/printersetup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGz(req, PrinterSetup_html_gz, PrinterSetup_html_gz_len, PrinterSetup_html_gz_mime);
  });

  server.on("/maintenance", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGz(req, Maintenance_html_gz, Maintenance_html_gz_len, Maintenance_html_gz_mime);
  });

  server.on("/vpn", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGzChunked(req, VpnSetup_html_gz, VpnSetup_html_gz_len, VpnSetup_html_gz_mime);
  });

  server.on("/ledtest", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGz(req, LedTest_html_gz, LedTest_html_gz_len, LedTest_html_gz_mime);
  });

  server.on("/style.css", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, Style_css_gz, Style_css_gz_len, Style_css_gz_mime);
  });

  server.on("/logo.svg", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, logo_svg_gz, logo_svg_gz_len, logo_svg_gz_mime);
  });

  server.on("/favicon.ico", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, logo_ico_gz, logo_ico_gz_len, logo_ico_gz_mime);
  });

  server.on("/backgroundCanvas.js", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, backgroundCanvas_js_gz, backgroundCanvas_js_gz_len, backgroundCanvas_js_gz_mime);
  });

  server.on("/footer.js", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, footer_js_gz, footer_js_gz_len, footer_js_gz_mime);
  });

  server.on("/netlist", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleNetlist(req);
  });

  server.on("/bblprinterdiscovery", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handlePrinterDiscovery(req);
  });

  server.on("/submitConfig", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleSubmitConfig(req);
  });

  server.on("/submitPrinterConfig", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleSubmitPrinterConfig(req);
  });

  server.on("/ledtestcmd", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleLedTestCmd(req);
  });

  server.on("/config/backup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    const bool pretty = req->hasParam("pretty");
    String out = settings.backup(false);
    JsonDocument backupDoc;
    if (deserializeJson(backupDoc, out) == DeserializationError::Ok) {
      JsonObject vpn = backupDoc["vpn"].to<JsonObject>();
      vpn.remove("private_key");
      vpn.remove("preshared_key");

      const VpnSecretStore::KeyMeta privateMeta = VpnSecretStore::privateKeyMeta();
      const VpnSecretStore::KeyMeta pskMeta = VpnSecretStore::presharedKeyMeta();
      vpn["hasPrivateKey"] = privateMeta.has;
      vpn["privateKeyFp"] = privateMeta.fingerprint;
      vpn["hasPresharedKey"] = pskMeta.has;
      vpn["presharedKeyFp"] = pskMeta.fingerprint;

      JsonObject meta = backupDoc["_meta"].to<JsonObject>();
      meta["vpnSecretsExcluded"] = true;
      meta["vpnSecretsNote"] = "VPN secrets are intentionally excluded from backup.";

      out = "";
      if (pretty) {
        serializeJsonPretty(backupDoc, out);
      } else {
        serializeJson(backupDoc, out);
      }
    }

    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
    r->addHeader("Content-Disposition", "attachment; filename=bambubeacon-backup.json");
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/config/restore", HTTP_POST,
    [&](AsyncWebServerRequest* req) {
      if (!wifiManager.isApMode()) {
        if (!isAuthorized(req)) {
          if (req->_tempObject) {
            delete (String*)req->_tempObject;
            req->_tempObject = nullptr;
          }
          return req->requestAuthentication();
        }
      }

      String* body = (String*)req->_tempObject;
      const bool ok = body && settings.restore(*body, true, true);
      if (body) {
        delete body;
        req->_tempObject = nullptr;
      }

      if (ok) {
        // Backup/restore intentionally excludes VPN secrets. Clear stored secrets
        // on restore to avoid stale credentials surviving a config restore.
        VpnSecretStore::clearAllSecrets();
        req->send(200, "application/json", "{\"success\":true}");
        scheduleRestart(600);
      } else {
        req->send(400, "application/json", "{\"success\":false}");
      }
    },
    nullptr,
    [&](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      if (!wifiManager.isApMode() && !isAuthorized(req)) return;
      String* body = (String*)req->_tempObject;
      if (!body) {
        body = new String();
        body->reserve(total);
        req->_tempObject = body;
      }
      body->concat((const char*)data, len);
    }
  );

  static bool mqttPausedForUpdate = false;

  server.on("/update", HTTP_POST,
    [&](AsyncWebServerRequest* req) {
      if (!wifiManager.isApMode()) {
        if (!isAuthorized(req)) return req->requestAuthentication();
      }
      const bool ok = !Update.hasError();
      if (!ok) {
        ledsCtrl.setOtaProgressManual(false, 255);
      }
      if (!ok && mqttPausedForUpdate) {
        mqttPausedForUpdate = false;
        if (WiFi.status() == WL_CONNECTED) bambu.connect();
      }
      req->send(ok ? 200 : 500, "application/json", ok ? "{\"success\":true}" : "{\"success\":false}");
      if (ok) scheduleRestart(2500);
    },
    [&](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      (void)filename;
      if (!wifiManager.isApMode() && !isAuthorized(req)) return;
      if (index == 0) {
        if (!mqttPausedForUpdate) {
          bambu.disconnect();
          mqttPausedForUpdate = true;
          webSerial.println("[MQTT] Paused for OTA");
        }
        ledsCtrl.setOtaProgressManual(true, 0);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(webSerial);
        }
      }
      if (Update.write(data, len) != len) {
        Update.printError(webSerial);
      }
      if (req && req->contentLength() > 0) {
        const size_t totalLen = req->contentLength();
        uint32_t done = (uint32_t)(index + len);
        if (done > totalLen) done = (uint32_t)totalLen;
        uint8_t pct = (uint8_t)((done * 100ULL) / totalLen);
        ledsCtrl.setOtaProgressManual(true, pct);
      }
      if (final) {
        const bool ok = Update.end(true);
        if (!ok) {
          Update.printError(webSerial);
          ledsCtrl.setOtaProgressManual(false, 255);
          if (mqttPausedForUpdate) {
            mqttPausedForUpdate = false;
            if (WiFi.status() == WL_CONNECTED) bambu.connect();
          }
        } else {
          ledsCtrl.setOtaProgressManual(true, 100);
        }
      }
    }
  );

  server.on("/ota/status", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    const String out = ota.statusJson();
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/ota/check", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    ota.requestCheck();
    const String out = ota.statusJson();
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/ota/update", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    ota.startUpdate();
    const String out = ota.statusJson();
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/netconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["deviceName"] = settings.get.deviceName();
    doc["ssid0"] = settings.get.wifiSsid0();
    doc["pass0"] = settings.get.wifiPass0();
    doc["bssid0"] = settings.get.wifiBssid0();
    doc["bssidLock"] = settings.get.wifiBssidLock();
    doc["ssid1"] = settings.get.wifiSsid1();
    doc["pass1"] = settings.get.wifiPass1();
    doc["ip"] = settings.get.staticIP();
    doc["subnet"] = settings.get.staticSN();
    doc["gateway"] = settings.get.staticGW();
    doc["dns"] = settings.get.staticDNS();
    doc["webUser"] = settings.get.webUIuser();
    // FIX: return the password, not the user name
    doc["webPass"] = settings.get.webUIPass();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/printerconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["printerIP"] = settings.get.printerIP();
    doc["printerUSN"] = settings.get.printerUSN();
    doc["printerAC"] = settings.get.printerAC();
    doc["hmsIgnore"] = settings.get.hmsIgnore();
    doc["ledSegments"] = settings.get.LEDSegments();
    doc["ledPerSeg"] = settings.get.LEDperSeg();
    doc["ledMaxCurrentmA"] = settings.get.LEDMaxCurrentmA();
    doc["ledColorOrder"] = settings.get.LEDColorOrder();
    doc["ledReverseOrder"] = settings.get.LEDReverseOrder();
    doc["idleTimeoutMin"] = settings.get.idleTimeoutMin();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/hmsignore.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["hmsIgnore"] = settings.get.hmsIgnore();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/setHmsIgnore", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    const String v = req->hasParam("hmsignore", true) ? req->getParam("hmsignore", true)->value() : "";
    settings.set.hmsIgnore(v);
    settings.save();
    bambu.reloadFromSettings();
    if (WiFi.status() == WL_CONNECTED) bambu.connect();
    req->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/hms.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    BambuMqttClient::HmsEvent events[20];
    const size_t n = bambu.getActiveEvents(events, 20);
    if (n == 0) {
      doc["present"] = false;
    } else {
      size_t best = 0;
      for (size_t i = 1; i < n; i++) {
        if ((uint8_t)events[i].severity > (uint8_t)events[best].severity) {
          best = i;
        } else if (events[i].severity == events[best].severity &&
                   events[i].lastSeenMs > events[best].lastSeenMs) {
          best = i;
        }
      }
      doc["present"] = true;
      doc["code"] = events[best].codeStr;
      doc["severity"] = (uint8_t)events[best].severity;
      doc["count"] = (uint32_t)events[best].count;
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/hmsignore/add", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    String code = req->hasParam("code", true) ? req->getParam("code", true)->value() : "";
    code.trim();
    code.toUpperCase();
    if (!code.length()) {
      return req->send(400, "application/json", "{\"success\":false}");
    }
    String current = settings.get.hmsIgnore();
    String currentUpper = current;
    currentUpper.toUpperCase();
    if (currentUpper.indexOf(code) < 0) {
      if (current.length() && current[current.length() - 1] != '\n') current += "\n";
      current += code;
      current += "\n";
      settings.set.hmsIgnore(current);
      settings.save();
      bambu.reloadFromSettings();
      if (WiFi.status() == WL_CONNECTED) bambu.connect();
    }
    req->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/ledconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["ledBrightness"] = settings.get.LEDBrightness();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/setLedBrightness", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    if (!req->hasParam("brightness", true)) {
      req->send(400, "application/json", "{\"success\":false}");
      return;
    }

    const String v = req->getParam("brightness", true)->value();
    long b = v.toInt();
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    settings.set.LEDBrightness((uint16_t)b);
    settings.save();
    ledsCtrl.setBrightness((uint8_t)b);

    req->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/info.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!isAuthorized(req)) return req->requestAuthentication();

    JsonDocument doc;
    doc["deviceName"] = settings.get.deviceName();
    doc["mode"] = wifiManager.isApMode() ? "AP" : "STA";
    doc["ip"] = wifiManager.isApMode() ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    doc["version"] = STRVERSION;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/vpn/import", HTTP_POST,
    [&](AsyncWebServerRequest* req) {
      auto cleanup = [&]() {
        if (req->_tempObject) {
          delete (VpnImportUploadState*)req->_tempObject;
          req->_tempObject = nullptr;
        }
      };
      auto sendError = [&](const String& reason) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"] = reason;
        String out;
        serializeJson(doc, out);
        req->send(400, "application/json", out);
        cleanup();
      };

      if (!wifiManager.isApMode()) {
        if (!isAuthorized(req)) {
          cleanup();
          return req->requestAuthentication();
        }
      }

      VpnImportUploadState* state = (VpnImportUploadState*)req->_tempObject;
      if (!state) {
        return sendError("No import payload received.");
      }
      if (state->error.length()) {
        return sendError(state->error);
      }
      if (!state->fileSeen) {
        return sendError("No file uploaded.");
      }
      if (!state->parser.finish() || state->parser.hasError()) {
        return sendError(state->parser.error().length() ? state->parser.error() : "Failed to parse WireGuard config.");
      }

      const VpnConfig current = VpnApi::loadConfigFromSettings();
      VpnConfig imported = current;
      VpnApi::WarningList warnings;
      VpnApi::ImportSummary summary;
      String importError;
      if (!VpnApi::buildConfigFromImport(state->parser.parsed(), current, &imported, &warnings, &summary, &importError)) {
        return sendError(importError.length() ? importError : "Import failed.");
      }

      if (VpnApi::isLikelyWireGuardKey(imported.privateKey)) {
        VpnSecretStore::setPrivateKey(imported.privateKey);
      }
      if (imported.presharedKey.length()) {
        if (VpnApi::isLikelyWireGuardKey(imported.presharedKey)) {
          VpnSecretStore::setPresharedKey(imported.presharedKey);
        }
      } else {
        VpnSecretStore::clearPresharedKey();
      }

      const VpnSecretStore::KeyMeta importedPrivateMeta = VpnSecretStore::privateKeyMeta();
      const VpnSecretStore::KeyMeta importedPskMeta = VpnSecretStore::presharedKeyMeta();

      JsonDocument doc;
      doc["ok"] = true;
      JsonArray warningsArray = doc["warnings"].to<JsonArray>();
      for (size_t i = 0; i < warnings.count; i++) {
        warningsArray.add(warnings.items[i]);
      }
      JsonObject sum = doc["summary"].to<JsonObject>();
      sum["hasPrivateKey"] = summary.hasPrivateKey;
      sum["peerCount"] = summary.peerCount;
      sum["allowedIpsCount"] = summary.allowedIpsCount;
      sum["fullTunnel"] = summary.fullTunnel;
      doc["enabled"] = summary.enabled;

      JsonObject cfg = doc["config"].to<JsonObject>();
      cfg["enabled"] = imported.enabled;
      cfg["local_ip"] = imported.localIp.toString();
      cfg["local_mask"] = imported.localMask.toString();
      cfg["local_port"] = imported.localPort;
      cfg["local_gateway"] = imported.localGateway.toString();
      cfg["endpoint_host"] = imported.endpointHost;
      cfg["endpoint_public_key"] = imported.endpointPublicKey;
      cfg["endpoint_port"] = imported.endpointPort;
      cfg["allowed_ip"] = imported.allowedIp.toString();
      cfg["allowed_mask"] = imported.allowedMask.toString();
      cfg["make_default"] = false;
      cfg["hasPrivateKey"] = importedPrivateMeta.has;
      cfg["privateKeyFp"] = importedPrivateMeta.fingerprint;
      cfg["privateKeyFpDisplay"] = importedPrivateMeta.displayFingerprint;
      cfg["hasPresharedKey"] = importedPskMeta.has;
      cfg["presharedKeyFp"] = importedPskMeta.fingerprint;
      cfg["presharedKeyFpDisplay"] = importedPskMeta.displayFingerprint;

      String out;
      serializeJson(doc, out);
      req->send(200, "application/json", out);
      cleanup();
    },
    [&](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      if (!wifiManager.isApMode() && !isAuthorized(req)) return;

      VpnImportUploadState* state = (VpnImportUploadState*)req->_tempObject;
      if (!state) {
        state = new VpnImportUploadState();
        req->_tempObject = state;
      }

      if (index == 0) {
        state->fileSeen = true;
        if (!VpnApi::isAllowedImportFilename(filename)) {
          state->error = "Invalid file type. Use .conf or .txt.";
          return;
        }
      }

      if (state->error.length()) return;

      if (!state->parser.feed(data, len)) {
        state->error = state->parser.error().length() ? state->parser.error() : "Failed to parse upload data.";
        return;
      }

      if (final) {
        if (!state->parser.finish() || state->parser.hasError()) {
          state->error = state->parser.error().length() ? state->parser.error() : "Failed to finalize parsing.";
        }
      }
    }
  );

  server.on("/api/vpn", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleGetVpnApi(req);
  });

  server.on("/api/vpn", HTTP_POST,
    [&](AsyncWebServerRequest* req) {
      if (!wifiManager.isApMode()) {
        if (!isAuthorized(req)) {
          if (req->_tempObject) {
            delete (String*)req->_tempObject;
            req->_tempObject = nullptr;
          }
          return req->requestAuthentication();
        }
      }

      String* body = (String*)req->_tempObject;
      String payload;
      if (body && body->length()) {
        payload = *body;
      }
      if (!payload.length() && req->hasParam("plain", true)) {
        payload = req->getParam("plain", true)->value();
      }
      if (body) {
        delete body;
        req->_tempObject = nullptr;
      }

      if (!payload.length()) {
        req->send(400, "application/json", "{\"success\":false,\"reason\":\"empty_body\"}");
        return;
      }

      handleSetVpnApi(req, payload);
    },
    nullptr,
    [&](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      (void)index;
      if (!wifiManager.isApMode() && !isAuthorized(req)) return;
      String* body = (String*)req->_tempObject;
      if (!body) {
        body = new String();
        body->reserve(total);
        req->_tempObject = body;
      }
      body->concat((const char*)data, len);
    }
  );

  server.onNotFound([&](AsyncWebServerRequest* req) {
    // Nice fallback: if in AP mode, redirect everything to setup page
    if (wifiManager.isApMode()) {
      req->redirect("/wifisetup");
      return;
    }
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  webSerial.println("[WEB] Server started");
}
