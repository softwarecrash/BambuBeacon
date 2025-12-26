# BambuBeacon

..... still under construction .....

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
- Some printed Parts
- BambuLab printer (make less sense without one)

## Wiring Notes ##
- Connect LED data to the GPIO defined by `LED_PIN` in `platformio.ini`
- Share ground between the ESP32 and the LED rings
- Power the LED rings from a stable 5V supply (2A or more recommended)
- Optional but recommended: place a small resistor (~330-470 Ohm) in series with the data line

## Quick Start ##
1. Build and flash the firmware with PlatformIO.
2. Power the device and connect to its Wi-Fi AP.
3. Open the setup page (http://192.168.4.1) and configure Wi-Fi, then reboot into STA mode.
4. Open Printer Setup and enter printer IP/USN/access key.
5. Set LED ring count (2 or 3), LEDs per ring (1-64), max LED current limit, and ring order, then save.
6. Verify status updates on the LED rings and check logs via WebSerial if needed.

## Default LED Settings ##
- LED segments: 3
- LEDs per ring: 12
- Brightness: 50 (0-255)
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