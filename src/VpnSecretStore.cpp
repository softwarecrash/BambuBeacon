#include "VpnSecretStore.h"

#include <Preferences.h>
#include <mbedtls/sha256.h>

namespace {
constexpr const char* kSecretNamespace = "vpnsec";
constexpr const char* kPrivateSecretKey = "priv_secret";
constexpr const char* kPrivateFpKey = "priv_fp";
constexpr const char* kPresharedSecretKey = "psk_secret";
constexpr const char* kPresharedFpKey = "psk_fp";

constexpr const char* kLegacyNamespace = "vpn";
constexpr const char* kLegacyPrivateKey = "private_key";
constexpr const char* kLegacyPresharedKey = "preshared_key";

bool gInitialized = false;

String trimCopy(const String& value) {
  String out = value;
  out.trim();
  return out;
}

String sha256Hex(const String& value) {
  const String trimmed = trimCopy(value);
  uint8_t hash[32] = {0};

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx,
                            reinterpret_cast<const uint8_t*>(trimmed.c_str()),
                            trimmed.length());
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  static constexpr char kHex[] = "0123456789abcdef";
  String out;
  out.reserve(64);
  for (size_t i = 0; i < sizeof(hash); i++) {
    out += kHex[(hash[i] >> 4) & 0x0F];
    out += kHex[hash[i] & 0x0F];
  }
  return out;
}

bool writeSecretPair(const char* secretKey, const char* fpKey, const String& secret) {
  const String trimmed = trimCopy(secret);
  if (!trimmed.length()) return false;

  Preferences prefs;
  if (!prefs.begin(kSecretNamespace, false)) return false;
  const bool ok1 = prefs.putString(secretKey, trimmed) > 0;
  const bool ok2 = prefs.putString(fpKey, sha256Hex(trimmed)) > 0;
  prefs.end();
  return ok1 && ok2;
}

bool readSecret(const char* secretKey, String* out) {
  if (!out) return false;
  Preferences prefs;
  if (!prefs.begin(kSecretNamespace, true)) {
    *out = "";
    return false;
  }
  *out = prefs.getString(secretKey, "");
  prefs.end();
  return out->length() > 0;
}

String readFingerprint(const char* fpKey) {
  Preferences prefs;
  if (!prefs.begin(kSecretNamespace, true)) return "";
  String fp = prefs.getString(fpKey, "");
  prefs.end();
  fp.toLowerCase();
  fp.trim();
  return fp;
}

bool ensureFingerprint(const char* secretKey, const char* fpKey) {
  String secret;
  if (!readSecret(secretKey, &secret)) return false;

  String fp = readFingerprint(fpKey);
  if (fp.length() == 64) return true;

  return writeSecretPair(secretKey, fpKey, secret);
}

bool clearSecretPair(const char* secretKey, const char* fpKey) {
  Preferences prefs;
  if (!prefs.begin(kSecretNamespace, false)) return false;
  prefs.remove(secretKey);
  prefs.remove(fpKey);
  prefs.end();
  return true;
}

void migrateLegacySecrets() {
  if (gInitialized) return;
  gInitialized = true;

  String existingPrivate;
  String existingPsk;
  const bool hasPrivate = readSecret(kPrivateSecretKey, &existingPrivate);
  const bool hasPsk = readSecret(kPresharedSecretKey, &existingPsk);

  Preferences legacy;
  if (!legacy.begin(kLegacyNamespace, false)) return;
  const String legacyPrivate = legacy.getString(kLegacyPrivateKey, "");
  const String legacyPsk = legacy.getString(kLegacyPresharedKey, "");

  if (!hasPrivate && trimCopy(legacyPrivate).length()) {
    writeSecretPair(kPrivateSecretKey, kPrivateFpKey, legacyPrivate);
  }
  if (!hasPsk && trimCopy(legacyPsk).length()) {
    writeSecretPair(kPresharedSecretKey, kPresharedFpKey, legacyPsk);
  }

  legacy.remove(kLegacyPrivateKey);
  legacy.remove(kLegacyPresharedKey);
  legacy.end();
}

}  // namespace

namespace VpnSecretStore {
String shortenFingerprint(const String& fullFingerprint) {
  String fp = fullFingerprint;
  fp.trim();
  if (fp.length() <= 16) return fp;

  String out;
  out.reserve(19);
  out += fp.substring(0, 8);
  out += "...";
  out += fp.substring(fp.length() - 8);
  return out;
}

bool fingerprintsMatch(const String& provided, const String& stored) {
  String a = provided;
  String b = stored;
  a.trim();
  b.trim();
  a.toLowerCase();
  b.toLowerCase();
  return a.length() > 0 && a == b;
}

bool loadPrivateKey(String* out) {
  migrateLegacySecrets();
  ensureFingerprint(kPrivateSecretKey, kPrivateFpKey);
  return readSecret(kPrivateSecretKey, out);
}

bool loadPresharedKey(String* out) {
  migrateLegacySecrets();
  ensureFingerprint(kPresharedSecretKey, kPresharedFpKey);
  return readSecret(kPresharedSecretKey, out);
}

KeyMeta privateKeyMeta() {
  migrateLegacySecrets();
  ensureFingerprint(kPrivateSecretKey, kPrivateFpKey);

  KeyMeta meta;
  String secret;
  meta.has = readSecret(kPrivateSecretKey, &secret);
  meta.fingerprint = readFingerprint(kPrivateFpKey);
  meta.displayFingerprint = shortenFingerprint(meta.fingerprint);
  return meta;
}

KeyMeta presharedKeyMeta() {
  migrateLegacySecrets();
  ensureFingerprint(kPresharedSecretKey, kPresharedFpKey);

  KeyMeta meta;
  String secret;
  meta.has = readSecret(kPresharedSecretKey, &secret);
  meta.fingerprint = readFingerprint(kPresharedFpKey);
  meta.displayFingerprint = shortenFingerprint(meta.fingerprint);
  return meta;
}

bool setPrivateKey(const String& key) {
  migrateLegacySecrets();
  return writeSecretPair(kPrivateSecretKey, kPrivateFpKey, key);
}

bool setPresharedKey(const String& key) {
  migrateLegacySecrets();
  return writeSecretPair(kPresharedSecretKey, kPresharedFpKey, key);
}

bool clearPrivateKey() {
  migrateLegacySecrets();
  return clearSecretPair(kPrivateSecretKey, kPrivateFpKey);
}

bool clearPresharedKey() {
  migrateLegacySecrets();
  return clearSecretPair(kPresharedSecretKey, kPresharedFpKey);
}

void clearAllSecrets() {
  clearPrivateKey();
  clearPresharedKey();
}
}  // namespace VpnSecretStore
