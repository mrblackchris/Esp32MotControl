# Getting started

[← Project overview](../README.md)

## Requirements

- A **classic ESP32** board. The sketch depends on its two MCPWM groups and checks the target at compile time.
- Arduino IDE with the ESP32 board package, or Arduino CLI. This project has been compiled with **Arduino ESP32 core 3.3.12** and board identifier `esp32:esp32:esp32`.
- A USB connection and the appropriate serial permissions on your computer.
- Python 3 for rebuilding the standalone HTML and embedded page.

The ESP32 provides logic signals for a gate driver. It does not directly power a motor. Refer to [Wiring & inputs](WIRING.md) for the signal connections. The software currently provides open-loop waveform control; it does not implement automatic V/f, current regulation or an acceleration ramp.

## 1. Configure Wi-Fi

In the project directory:

```sh
cp wifi_config.example.h wifi_config.h
```

Edit `wifi_config.h`:

```cpp
#pragma once
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

`wifi_config.h` is ignored by Git. Without that file, the firmware still builds using the example placeholders. It will not connect to your network with those placeholders. Wi-Fi settings are compiled into the firmware; there is currently no network-settings form or access-point provisioning mode.

## 2. Build and upload

### Arduino IDE

Open `Esp32MotControl.ino`, install/select the classic ESP32 board, select your serial port, then build and upload. The generated `web_page.h` is already included in the project. Rebuild the web assets with `python3 web/build.py` if you edit the web sources.

### Arduino CLI

If the board package is not installed:

```sh
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

Build, find the serial port, then upload:

```sh
python3 web/build.py
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli board list
arduino-cli upload --fqbn esp32:esp32:esp32 --port /dev/ttyUSB0 .
```

Replace `/dev/ttyUSB0` with your board's port. Uploading replaces the board's firmware.

## 3. Connect over Wi-Fi

1. Open a serial monitor at **115200 baud**.
2. Wait for `Web interface: http://...` after the ESP32 connects.
3. Open that address in a browser on the same reachable network.

The ESP32 serves the interface directly on port 80. It uses HTTP. The motor outputs boot OFF in Remote mode; GPIO25 runs independently. The interface has no authentication layer, so use it on a trusted network.

Alternatively, open `motor-control.html` locally, enter the ESP32's HTTP address, and click **Connect Wi-Fi**. Browser restrictions on requests to local devices can affect this path; opening the ESP32-hosted page is the direct option.

## 4. Connect over USB serial

Open the standalone [motor-control.html](../motor-control.html) in Chrome/Chromium with Web Serial support, then click **Connect USB serial** and select the ESP32 port.

If your browser does not allow Web Serial from a local file, serve the project on localhost:

```sh
python3 -m http.server 8000 --bind 127.0.0.1
```

Then open `http://localhost:8000/motor-control.html`, click **Connect USB serial**, and select the port. Close another serial monitor before connecting: one client should own the serial port at a time. Opening the USB connection can reset the board; the page retries its initial state read to allow for startup.

The localhost server only serves files. It is not an ESP32 API emulator; to use Wi-Fi, enter the real device address and click **Connect Wi-Fi**. The page initially tries its own HTTP origin, so a failed initial status read on localhost is expected.

## 5. Use the controls

**Remote:** read the settings, edit the form, and press **Apply remote settings**. Changes in the form are not sent until applied. Stop before applying a different direction. **Read settings into form** discards pending form edits and reloads applied values.

**Local:** first apply modulation, carrier and dead time in Remote mode. Select Local and apply the source. Observe the run switch OFF, set the potentiometer above zero, then switch ON. A direction change while running stops the outputs; cycle OFF → ON to restart in the new direction.

The status area refreshes every second and includes all three analog input voltages. If zero frequency stops the motor in Local mode, raising the potentiometer alone does not restart it; cycle the run switch.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No Wi-Fi address | Credentials in `wifi_config.h`, network reachability and serial boot output |
| USB button is unavailable | Web Serial support and a secure context such as localhost |
| USB port cannot open | Close serial monitors and other clients; check OS serial permissions |
| Voltage fields are missing / undefined | Flash the current firmware and reload the current HTML page |
| `/set` returns 409 | Select Remote mode, or stop before changing direction |
| Local switch is ON but outputs stay OFF | OFF → ON is required after source selection, Stop, reversal or a zero-frequency stop |
| Potentiometer voltage drifts when unplugged | GPIO34 is floating; this is not reliable connection detection |
| Status is unavailable | Output state is unknown to the browser; reconnect and read state before retrying a command |

For command-level access, see the [Control API](CONTROL_API.md).
