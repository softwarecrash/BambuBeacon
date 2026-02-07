# WireGuard Integration Notes

## Reference used
- `https://github.com/codm/czc-fw` (cod.m Zigbee coordinator firmware)
- `https://github.com/Tinkerforge/WireGuard-ESP32-Arduino` (WireGuard library used by `codm/czc-fw`)

## Reused approach
- WireGuard netif lifecycle based on cod.m/Tinkerforge implementation:
  - create netif with `wireguardif_init`
  - add peer via `wireguardif_add_peer`
  - connect with `wireguardif_connect`
  - remove/shutdown via `wireguardif_disconnect`, `wireguardif_remove_peer`, `wireguardif_shutdown`
- Same WireGuard field model as cod.m VPN page:
  - local interface IP/mask/port/gateway
  - local private key
  - endpoint host/public key/port
  - allowed IP/mask
  - optional preshared key
- Runtime connection checks use `wireguardif_peer_is_up` pattern (similar to cod.m `wgLoop` status checks).

## BambuBeacon-specific differences
- Non-blocking behavior was enforced:
  - no retry loops with blocking delays for DNS/connection setup
  - start retries are time-based in `update()`
- Added explicit Wi-Fi coupling:
  - stop VPN when Wi-Fi disconnects
  - auto-restart when Wi-Fi reconnects
- Added security behavior for web API:
  - keys are never returned by API
  - API returns only key presence and SHA-256 fingerprints
  - secrets are stored separately from normal config JSON in NVS
  - legacy key reveal flow was removed; no API/UI path returns plaintext keys
- Added AsyncWebServer JSON API (`/api/vpn`) and BambuBeacon-native page routing/UI integration (`/vpn`).
- Added streaming WireGuard `.conf` import endpoint (`/api/vpn/import`) with FRITZ!Box-compatible parsing.
- Added strict split-tunnel safety guard:
  - full-tunnel entries (`0.0.0.0/0`, `::/0`) are never applied
  - imports drop full-tunnel entries and require an RFC1918 printer subnet
  - runtime blocks unsafe routes with status `DISCONNECTED (unsafe route)`
  - config restore clears stored VPN secrets because backups intentionally exclude secret material

## Keepalive and status
- Persistent keepalive is set to 25 seconds for the configured WireGuard peer.
- Handshake age is exposed when available; otherwise API returns sentinel/unavailable values.
