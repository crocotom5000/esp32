# Wi-Fi Dashboard

A clock, current weather, and Wi-Fi status screen for the
**Waveshare ESP32-S3-Touch-AMOLED-1.64** (280×456 AMOLED).

- Large 24-hour clock, date, time zone, and seconds; NTP time sync with DST rules.
- Current temperature, feels-like temperature, humidity, wind, and weather icon.
- Network name, IP address, signal bars, and RSSI.
- **Tap anywhere** to refresh weather (limited to once per 10 seconds).
- Automatic weather updates every 10 minutes; failures retry every minute.
- Wi-Fi reconnects automatically. Last good weather stays visible on failures,
  with its fetch age; data over 30 minutes old is marked `!`.
- HTTPS certificate verification using the ESP32 core's built-in CA bundle.
- Network requests run in a separate task, keeping the screen and touch responsive.

## Configuration

From this directory:

```bash
cp config.example.h config.h
```

Edit **`config.h`**:

| Setting | Meaning |
| --- | --- |
| `WIFI_SSID`, `WIFI_PASSWORD` | Your 2.4 GHz network credentials |
| `LOCATION_NAME` | Display label (ASCII; long names are truncated) |
| `LATITUDE`, `LONGITUDE` | Weather coordinates in decimal degrees |
| `TIMEZONE` | POSIX time-zone rule, including DST if applicable |
| `DISPLAY_BRIGHTNESS` | AMOLED brightness, 0–255 |

The example location is **London**, not an automatically detected location.
Coordinates control weather; `TIMEZONE` independently controls the clock.
Use a POSIX rule such as `GMT0BST,M3.5.0/1,M10.5.0`, **not** `Europe/London`.
More examples are in `config.example.h`. Weather uses Celsius and km/h.

`config.h` is ignored by Git, but credentials are still embedded in the compiled
firmware: don't share that file or your firmware binaries. No API key is needed.
Without `config.h` (or with an empty SSID), the sketch builds and displays setup
instructions without connecting to Wi-Fi.

## Build and upload

The profile pins ESP32 core **3.3.12** and **GFX Library for Arduino 1.6.9**,
and enables the board's PSRAM for the framebuffer, leaving internal RAM for TLS.
WiFi, HTTPClient, NetworkClientSecure, and cJSON come with the ESP32 core.

```bash
arduino-cli compile --profile esp32 .
arduino-cli upload --profile esp32 --port /dev/esp32 .
```

Adjust the port if needed (`arduino-cli board list`). Uploading replaces the
sketch currently running on the board.

## Hardware

Same wiring as the other apps in this repository:

- CO5300: CS 46, SCK 10, D0–D3 11–14, RST 21; 20 px column offset.
- FT3168 touch: SDA 47, SCL 48, I²C address `0x38`; no axis swap.
- One 280×456 RGB565 off-screen framebuffer (~250 KiB), composed before sending
  to the display to avoid visible clear/redraw flicker.

## Tests

Pure C++ model tests can run without a board or Arduino libraries, from this
directory:

```bash
c++ -std=c++11 -Wall -Wextra -Werror tests/weather_model_test.cpp -o /tmp/opencode/weather_model_test
/tmp/opencode/weather_model_test
```

With the native cJSON development library installed, test the response parser
as well (optionally pass a saved Open-Meteo JSON response to the executable):

```bash
c++ -std=c++11 -Wall -Wextra -Werror tests/weather_json_test.cpp $(pkg-config --cflags --libs libcjson) -o /tmp/opencode/weather_json_test
/tmp/opencode/weather_json_test
```

On-device checks: boot without config; connect with valid credentials; verify
local time/DST and weather; tap to refresh; turn off the access point and confirm
the clock keeps ticking, cached weather remains, and Wi-Fi reconnects later.
Wrong credentials should show disconnected status rather than hang at boot.

## Network and weather notes

Internet access, DNS, and NTP (UDP 123) are required. Weather waits for NTP time
before opening HTTPS, because certificate validation needs a correct clock.
Captive portals and enterprise Wi-Fi are not supported. If the clock is stuck at
`--:--`, check whether NTP is blocked. A weather HTTP error or connection failure
appears in the footer and retries automatically.

Weather data: [Open-Meteo](https://open-meteo.com/), [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
The free endpoint is for non-commercial use, subject to Open-Meteo's limits and
terms. Current conditions are model estimates, not an onboard sensor reading.
The displayed age is time since the last successful fetch, not observation age.
