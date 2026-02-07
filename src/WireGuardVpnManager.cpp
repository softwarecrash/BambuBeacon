#include "WireGuardVpnManager.h"

#include <WiFi.h>
#include <esp_netif.h>
#include <lwip/err.h>
#include <lwip/ip.h>
#include <lwip/netdb.h>

extern "C" {
#include "wireguard-platform.h"
#include "wireguard.h"
#include "wireguardif.h"
}

namespace {
constexpr uint32_t kPeerCheckIntervalMs = 2000;
constexpr uint32_t kStartRetryIntervalMs = 5000;
constexpr uint16_t kPersistentKeepaliveSeconds = 25;

struct NetifAddAndUpParams {
  const ip4_addr_t* ipaddr;
  const ip4_addr_t* netmask;
  const ip4_addr_t* gateway;
  struct netif** output;
  struct netif* netifStorage;
  void* state;
};

static esp_err_t netifAddAndUpInLwipCtx(void* ctx) {
  auto* params = static_cast<NetifAddAndUpParams*>(ctx);
  *params->output = netif_add(
      params->netifStorage,
      params->ipaddr,
      params->netmask,
      params->gateway,
      params->state,
      &wireguardif_init,
      &ip_input);
  if (!*params->output) {
    return ESP_FAIL;
  }
  netif_set_up(*params->output);
  return ESP_OK;
}

static esp_err_t shutdownAndRemoveInLwipCtx(void* ctx) {
  auto* netif = static_cast<struct netif*>(ctx);
  wireguardif_shutdown(netif);
  netif_remove(netif);
  return ESP_OK;
}

static bool isLikelyWireGuardKey(const String& value) {
  const size_t len = value.length();
  if (len < 40 || len > 80) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    const char c = value[i];
    const bool ok =
        (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '+' || c == '/' || c == '=' ||
        c == '-' || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

static bool isFullTunnelRoute(const IPAddress& allowedIp, const IPAddress& allowedMask) {
  (void)allowedIp;
  return allowedMask == IPAddress(0, 0, 0, 0);
}
}  // namespace

WireGuardVpnManager::WireGuardVpnManager() {
  memset(&_wgNetifStruct, 0, sizeof(_wgNetifStruct));
  setStatus("DISCONNECTED", "disabled");
}

bool WireGuardVpnManager::begin(const VpnConfig& cfg) {
  _cfg = cfg;
  _enabled = cfg.enabled;
  _lastPeerCheckMs = 0;
  _lastStartAttemptMs = 0;
  _lastHandshakeAgeSeconds = UINT32_MAX;

  stopTunnel(false, _enabled ? "reconfigure" : "disabled");

  if (!_enabled) {
    return true;
  }

  String reason;
  if (!validateConfig(&reason)) {
    setStatus("DISCONNECTED", reason.c_str());
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    setStatus("DISCONNECTED", "WiFi disconnected");
    return true;
  }

  return startTunnel();
}

void WireGuardVpnManager::end() {
  _enabled = false;
  stopTunnel(true, "disabled");
}

void WireGuardVpnManager::update() {
  if (!_enabled) {
    if (_initialized) {
      stopTunnel(false, "disabled");
    }
    return;
  }

  String reason;
  if (!validateConfig(&reason)) {
    if (_initialized) {
      stopTunnel(false, "invalid config");
    }
    setStatus("DISCONNECTED", reason.c_str());
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (_initialized) {
      stopTunnel(false, "WiFi disconnected");
    } else {
      setStatus("DISCONNECTED", "WiFi disconnected");
    }
    return;
  }

  const uint32_t nowMs = millis();

  if (!_initialized) {
    if (_lastStartAttemptMs == 0 || (uint32_t)(nowMs - _lastStartAttemptMs) >= kStartRetryIntervalMs) {
      startTunnel();
    } else {
      setStatus("CONNECTING", "retry scheduled");
    }
    return;
  }

  if (_lastPeerCheckMs == 0 || (uint32_t)(nowMs - _lastPeerCheckMs) >= kPeerCheckIntervalMs) {
    _lastPeerCheckMs = nowMs;
    _connected = checkPeerUp();
    if (_connected) {
      _lastHandshakeAgeSeconds = readHandshakeAgeSeconds();
      setStatus("CONNECTED", nullptr);
    } else {
      _lastHandshakeAgeSeconds = UINT32_MAX;
      setStatus("CONNECTING", "awaiting handshake");
    }
  }
}

bool WireGuardVpnManager::isEnabled() const {
  return _enabled;
}

bool WireGuardVpnManager::isConnected() const {
  return _connected;
}

uint32_t WireGuardVpnManager::lastHandshakeSeconds() const {
  if (!_initialized || !_connected) {
    return 0;
  }
  return _lastHandshakeAgeSeconds;
}

const char* WireGuardVpnManager::statusText() const {
  return _statusText;
}

bool WireGuardVpnManager::startTunnel() {
  _lastStartAttemptMs = millis();

  ip_addr_t endpointIp = IPADDR4_INIT_BYTES(0, 0, 0, 0);
  if (!_cfg.endpointHost.length()) {
    setStatus("DISCONNECTED", "missing endpoint host");
    return false;
  }

  IPAddress endpointAsIp;
  bool endpointResolved = false;
  if (endpointAsIp.fromString(_cfg.endpointHost)) {
    endpointIp = IPADDR4_INIT(static_cast<uint32_t>(endpointAsIp));
    endpointResolved = true;
  } else {
    struct addrinfo* result = nullptr;
    struct addrinfo hint = {};
    hint.ai_family = AF_INET;
    if (lwip_getaddrinfo(_cfg.endpointHost.c_str(), nullptr, &hint, &result) == 0 && result && result->ai_addr) {
      const auto* addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
      inet_addr_to_ip4addr(ip_2_ip4(&endpointIp), &addr->sin_addr);
      endpointResolved = true;
    }
    if (result) {
      lwip_freeaddrinfo(result);
    }
  }

  if (!endpointResolved || ip_addr_isany(&endpointIp)) {
    setStatus("DISCONNECTED", "endpoint DNS failed");
    return false;
  }

  struct wireguardif_init_data initData = {};
  initData.private_key = _cfg.privateKey.c_str();
  initData.listen_port = _cfg.localPort;
  initData.bind_netif = nullptr;

  struct wireguardif_peer peer = {};
  wireguardif_peer_init(&peer);
  peer.public_key = _cfg.endpointPublicKey.c_str();
  peer.preshared_key = _cfg.presharedKey.length() ? _cfg.presharedKey.c_str() : nullptr;
  peer.endpoint_ip = endpointIp;
  peer.endport_port = _cfg.endpointPort;
  peer.allowed_ip = IPADDR4_INIT(static_cast<uint32_t>(_cfg.allowedIp));
  peer.allowed_mask = IPADDR4_INIT(static_cast<uint32_t>(_cfg.allowedMask));
  peer.keep_alive = kPersistentKeepaliveSeconds;

  const ip_addr_t localIp = IPADDR4_INIT(static_cast<uint32_t>(_cfg.localIp));
  const ip_addr_t localMask = IPADDR4_INIT(static_cast<uint32_t>(_cfg.localMask));
  const ip_addr_t localGateway = IPADDR4_INIT(static_cast<uint32_t>(_cfg.localGateway));

  NetifAddAndUpParams params = {
      ip_2_ip4(&localIp),
      ip_2_ip4(&localMask),
      ip_2_ip4(&localGateway),
      &_wgNetif,
      &_wgNetifStruct,
      &initData,
  };

  if (esp_netif_tcpip_exec(netifAddAndUpInLwipCtx, &params) != ESP_OK || !_wgNetif) {
    setStatus("DISCONNECTED", "netif init failed");
    return false;
  }

  wireguard_platform_init();
  _wireguardPeerIndex = WIREGUARDIF_INVALID_INDEX;
  if (wireguardif_add_peer(_wgNetif, &peer, &_wireguardPeerIndex) != ERR_OK ||
      _wireguardPeerIndex == WIREGUARDIF_INVALID_INDEX) {
    esp_netif_tcpip_exec(shutdownAndRemoveInLwipCtx, _wgNetif);
    _wgNetif = nullptr;
    _wireguardPeerIndex = WIREGUARDIF_INVALID_INDEX;
    setStatus("DISCONNECTED", "peer init failed");
    return false;
  }

  if (wireguardif_connect(_wgNetif, _wireguardPeerIndex) != ERR_OK) {
    wireguardif_remove_peer(_wgNetif, _wireguardPeerIndex);
    esp_netif_tcpip_exec(shutdownAndRemoveInLwipCtx, _wgNetif);
    _wgNetif = nullptr;
    _wireguardPeerIndex = WIREGUARDIF_INVALID_INDEX;
    setStatus("DISCONNECTED", "connect failed");
    return false;
  }

  _initialized = true;
  _connected = false;
  _lastPeerCheckMs = 0;
  _lastHandshakeAgeSeconds = UINT32_MAX;
  setStatus("CONNECTING", "starting");
  return true;
}

void WireGuardVpnManager::stopTunnel(bool disableManager, const char* reason) {
  if (_wgNetif && _wireguardPeerIndex != WIREGUARDIF_INVALID_INDEX) {
    wireguardif_disconnect(_wgNetif, _wireguardPeerIndex);
    wireguardif_remove_peer(_wgNetif, _wireguardPeerIndex);
  }
  _wireguardPeerIndex = WIREGUARDIF_INVALID_INDEX;

  if (_wgNetif) {
    esp_netif_tcpip_exec(shutdownAndRemoveInLwipCtx, _wgNetif);
    _wgNetif = nullptr;
  }

  _initialized = false;
  _connected = false;
  _lastPeerCheckMs = 0;
  _lastHandshakeAgeSeconds = UINT32_MAX;

  if (disableManager) {
    _enabled = false;
  }
  setStatus("DISCONNECTED", reason);
}

bool WireGuardVpnManager::checkPeerUp() {
  if (!_initialized || !_wgNetif || _wireguardPeerIndex == WIREGUARDIF_INVALID_INDEX) {
    return false;
  }
  ip_addr_t peerIp = IPADDR4_INIT_BYTES(0, 0, 0, 0);
  uint16_t peerPort = 0;
  return wireguardif_peer_is_up(_wgNetif, _wireguardPeerIndex, &peerIp, &peerPort) == ERR_OK;
}

uint32_t WireGuardVpnManager::readHandshakeAgeSeconds() const {
  if (!_initialized || !_wgNetif || _wireguardPeerIndex == WIREGUARDIF_INVALID_INDEX) {
    return UINT32_MAX;
  }

  const auto* device = static_cast<const struct wireguard_device*>(_wgNetif->state);
  if (!device || _wireguardPeerIndex >= WIREGUARD_MAX_PEERS) {
    return UINT32_MAX;
  }

  const auto& peer = device->peers[_wireguardPeerIndex];
  if (!peer.valid || !peer.curr_keypair.valid) {
    return UINT32_MAX;
  }

  return (uint32_t)(millis() - peer.curr_keypair.keypair_millis) / 1000U;
}

bool WireGuardVpnManager::validateConfig(String* reason) const {
  auto fail = [&](const char* why) -> bool {
    if (reason) {
      *reason = why;
    }
    return false;
  };

  if (_cfg.localIp == IPAddress(0, 0, 0, 0)) {
    return fail("invalid local_ip");
  }
  if (_cfg.localMask == IPAddress(0, 0, 0, 0)) {
    return fail("invalid local_mask");
  }
  if (_cfg.localPort == 0) {
    return fail("invalid local_port");
  }
  if (_cfg.endpointPort == 0) {
    return fail("invalid endpoint_port");
  }
  if (!_cfg.endpointHost.length()) {
    return fail("missing endpoint_host");
  }
  if (!isLikelyWireGuardKey(_cfg.privateKey)) {
    return fail("invalid private_key");
  }
  if (!isLikelyWireGuardKey(_cfg.endpointPublicKey)) {
    return fail("invalid endpoint_public_key");
  }
  if (_cfg.presharedKey.length() && !isLikelyWireGuardKey(_cfg.presharedKey)) {
    return fail("invalid preshared_key");
  }
  if (_cfg.makeDefault || isFullTunnelRoute(_cfg.allowedIp, _cfg.allowedMask)) {
    return fail("unsafe route");
  }
  return true;
}

void WireGuardVpnManager::setStatus(const char* base, const char* reason) {
  if (reason && reason[0]) {
    snprintf(_statusText, sizeof(_statusText), "%s (%s)", base, reason);
  } else {
    snprintf(_statusText, sizeof(_statusText), "%s", base);
  }
}
