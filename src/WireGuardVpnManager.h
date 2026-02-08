#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <lwip/netif.h>
#include <freertos/FreeRTOS.h>

struct VpnConfig {
  bool enabled = false;
  IPAddress localIp;
  IPAddress localMask = IPAddress(255, 255, 255, 0);
  uint16_t localPort = 33333;
  IPAddress localGateway = IPAddress(0, 0, 0, 0);
  String privateKey;
  String endpointHost;
  String endpointPublicKey;
  uint16_t endpointPort = 0;
  IPAddress allowedIp = IPAddress(0, 0, 0, 0);
  IPAddress allowedMask = IPAddress(0, 0, 0, 0);
  bool makeDefault = false;
  String presharedKey;
};

class WireGuardVpnManager {
public:
  WireGuardVpnManager();

  bool begin(const VpnConfig& cfg);
  void end();
  void update();

  bool isEnabled() const;
  bool isConnected() const;
  uint32_t lastHandshakeSeconds() const;
  const char* statusText() const;

private:
  bool enterBusy();
  void leaveBusy();

  bool startTunnel();
  void stopTunnel(bool disableManager, const char* reason);
  bool checkPeerUp();
  uint32_t readHandshakeAgeSeconds() const;
  bool validateConfig(String* reason) const;
  void setStatus(const char* base, const char* reason);

  VpnConfig _cfg;
  bool _enabled = false;
  bool _initialized = false;
  bool _connected = false;
  uint32_t _lastPeerCheckMs = 0;
  uint32_t _lastStartAttemptMs = 0;
  uint32_t _lastHandshakeAgeSeconds = UINT32_MAX;
  char _statusText[96];

  struct netif _wgNetifStruct;
  struct netif* _wgNetif = nullptr;
  uint8_t _wireguardPeerIndex = 0xFF;
  portMUX_TYPE _busyMux = portMUX_INITIALIZER_UNLOCKED;
  bool _busy = false;
};
