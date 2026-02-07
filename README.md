[![GitHub release](https://img.shields.io/github/release/softwarecrash/bambuBeacon?include_prereleases=&sort=semver&color=blue)](https://github.com/softwarecrash/BambuBeacon/releases/latest) [![Discord](https://img.shields.io/discord/1007020337482973254?logo=discord&label=Discord)](https://discord.gg/Xcm6dfxMqp)

<img src='docs/gfx/BambuBeacon_Brand_COLOR.svg' wight='100%'>


## Project Description ##
BambuBeacon is an ESP32-based status light for BambuLab printers. It connects to your printer, listens for status updates, and drives multi-ring WS2812 LEDs to visualize printer state, progress, and connectivity in real time. The project includes a built-in web UI for setup, Wi-Fi configuration, and device management.

## Features ##
- Web-based setup for printer IP/USN/access key and device settings
- Wi-Fi AP mode for first-time configuration and recovery
- Real-time status visualization across 2 or 3 LED rings
- Adjustable LED brightness, per-ring LED counts, and max LED current limit
- Configurable ring order (top-to-bottom or bottom-to-top from the controller)
- DHCP-friendly printer discovery and tracking
- WebSerial console for live logs and troubleshooting
- JSON backup/restore of configuration
- OTA Firmware Updates

## Parts you need ##
- 3x 12bit WS2812 LED Ring
- 1x Wemos D1 Mini ESP32 or Athom Slim Wled controller or ESP32-C3 Nano
- Soldering Iron
- some wires
- USB power supply >= 500mA
- [Some printed Parts](https://makerworld.com/de/models/2163021-bambubeacon)
- BambuLab printer (make less sense without one)

## Wiring Notes ##
- Connect LED data to the GPIO defined by `LED_PIN` in `platformio.ini` for wemos d1 esp32 data pin is GPIO16 for ESp32C3 variant GPIO10
- <img width="1460" height="769" alt="image" src="https://github.com/user-attachments/assets/7cda282c-af30-4cba-89c4-3aa5603c3871" />

- Share ground between the ESP32 and the LED rings
- Power the LED rings from a stable 5V supply (2A or more recommended)
- Optional but recommended: place a small resistor (~330-470 Ohm) in series with the data line
- For LED test go to <BambuBeacon-IP>/ledtest

## Quick Start ##
1. Flash via the Web Flasher: https://softwarecrash.github.io/BambuBeacon/ (recommended), or build and flash the firmware with PlatformIO.
2. Power the device and connect to the Wi-Fi AP BambuBeacon-xxxxx.
3. Open the setup page (http://192.168.4.1) and configure Wi-Fi, Save and then go to http://bambubeacon.local or http://<the-name-you-given.local.
4. Open Printer Setup and enter printer IP/USN/access key.
5. Set LED ring count (2 or 3), LEDs per ring (1-64), max LED current limit, and ring order, then save.
6. Verify status updates on the LED rings and check logs via WebSerial if needed.

## Default LED Settings ##
- LED segments: 3
- LEDs per ring: 12
- Brightness: 50 (0-100)
- Max LED current: 500 mA (range 100-5000 mA, 5V assumed)
- Ring order: Top -> Middle -> Bottom

## LED Ring Behavior ##
- Ring 0 (top): OK/working = green solid; paused = green pulse; error/fatal = two red opposite LEDs rotating; finished = green comet laps with pause.
- Ring 1 (middle): Heating = orange pulse; cooling = blue pulse; paused = amber solid; warning = amber pulse; printing = green ring with a dim rotating gap.
- Ring 2 (bottom): Wi-Fi reconnect = purple blink; download progress = blue fill; print progress = green fill.

Color legend:
- Green: OK/working/progress
- Amber/Orange: heating/warnings/paused
- Red: error/fatal
- Blue: cooling/download
- Purple: Wi-Fi reconnect

## WireGuard VPN ##
BambuBeacon supports a WireGuard VPN client to reach printers in remote subnets.

### Key generation example
Use any WireGuard-capable host:

```bash
wg genkey | tee bb-private.key | wg pubkey > bb-public.key
wg genpsk > bb-psk.key
```

Keep private keys secret. Never share `bb-private.key` publicly.

### Sample VPN config (`POST /api/vpn`)
```json
{
  "enabled": true,
  "local_ip": "10.66.0.2",
  "local_mask": "255.255.255.0",
  "local_port": 33333,
  "local_gateway": "0.0.0.0",
  "private_key": "<base64-private-key>",
  "endpoint_host": "vpn.example.com",
  "endpoint_public_key": "<base64-peer-public-key>",
  "endpoint_port": 51820,
  "allowed_ip": "192.168.50.0",
  "allowed_mask": "255.255.255.0",
  "preshared_key": "<optional-base64-psk>"
}
```

Notes:
- Only the `allowed_ip/allowed_mask` printer subnet is routed through the tunnel (split tunnel).
- Full-tunnel routes (`0.0.0.0/0` or `::/0`) are rejected for safety.
- Private and preshared keys are masked in `GET /api/vpn` unless `?reveal=1` is explicitly requested.

### Importing WireGuard configs (FRITZ!Box example)
The VPN page supports importing a standard WireGuard client file (`.conf`).

Example flow:
1. Export a WireGuard client config from FRITZ!Box.
2. Open BambuBeacon `VPN` page and select the file in `Import WireGuard config (.conf)`.
3. Click `Import` to load values into the form.
4. Review and click `Save` to store and apply settings.

Import behavior:
- Import only fills the form; it does not save or apply automatically.
- If multiple `AllowedIPs` are present, the first RFC1918 subnet is used and additional entries are reported as warnings.
- If `0.0.0.0/0` is present, it is ignored with a warning to keep local access safe.
- If no RFC1918 printer subnet remains after filtering full-tunnel entries, import is rejected.

### Testing checklist
- VPN disabled: baseline behavior unchanged.
- VPN enabled with valid config: status reaches `CONNECTED`, remote allowed subnet is reachable.
- Wi-Fi reconnect: VPN restarts automatically and reconnects.
- Wrong config: status stays `DISCONNECTED (...)` with concise reason.
- UI responsiveness: Web UI remains responsive while VPN is connecting/retrying.

Spezial thanks to [@NeoRame](https://github.com/NeoRame) for Logo and Brand
