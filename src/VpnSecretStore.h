#pragma once

#include <Arduino.h>

namespace VpnSecretStore {
struct KeyMeta {
  bool has = false;
  String fingerprint;
  String displayFingerprint;
};

bool loadPrivateKey(String* out);
bool loadPresharedKey(String* out);

KeyMeta privateKeyMeta();
KeyMeta presharedKeyMeta();

bool setPrivateKey(const String& key);
bool setPresharedKey(const String& key);
bool clearPrivateKey();
bool clearPresharedKey();
void clearAllSecrets();

String shortenFingerprint(const String& fullFingerprint);
bool fingerprintsMatch(const String& provided, const String& stored);
}  // namespace VpnSecretStore

