# Bluetooth Trackpad

Turn the **Waveshare ESP32-S3-Touch-AMOLED-1.64** touchscreen into a wireless
trackpad for a computer. The board is a **Bluetooth LE HID mouse peripheral**,
not a mouse host like the separate `BluetoothMouse` sketch. No companion app or
Wi-Fi connection is needed.

## Pair and use

1. Build and upload the sketch below.
2. Open your computer's Bluetooth settings and pair **ESP32 Trackpad**. Accept
   the pairing request if prompted; no PIN is required.
3. Wait for **CONNECTED** on the board, then move a finger in the large pad.

- **Slide:** relative cursor movement; lift and reposition without moving the
  cursor. Movement is scaled 2×; the computer's pointer speed also applies.
- **Quick tap:** left click. Two quick taps work as a double-click, subject to
  the computer's double-click timing.
- **LEFT / RIGHT:** tap for a left/right click.
- **DRAG:** tap to hold the left button, move in the pad, then tap DRAG again
  to release. Finger lifts let you reposition without dropping the drag.
- **Right-side strip:** slide down to scroll down, up to scroll up. Your
  computer's natural-scrolling preference can reverse this.

Taps must be shorter than 250 ms and move no more than eight screen pixels.
Controls use one finger; two-finger gestures and native OS multitouch gestures
are not implemented. This intentionally presents as a mouse, not a Precision
Touchpad. Keep the board in portrait orientation, with the title at the top.

The board advertises on startup and after disconnection. Pairing keys survive
normal reboots/uploads; keep **Erase All Flash Before Sketch Upload** disabled
and the partition layout unchanged. Reconnection is initiated by the computer;
select Connect in its Bluetooth settings if it does not reconnect automatically.
Only one computer should connect at a time. To switch computers, disconnect the
old one first and pair from the new one.

Requires a computer/adapter with **Bluetooth LE HID** support (modern Linux,
Windows, or macOS). Bluetooth Classic-only adapters will not work. Pairing uses
encrypted Just Works bonding without man-in-the-middle authentication. Pair in
a trusted environment. There is no on-screen pairing confirmation or access
restriction to a single saved computer.

If pairing keys are lost on either side, remove/forget **ESP32 Trackpad** in
the computer's Bluetooth settings and pair again. Serial output at **115200**
reports connection and authentication results. **SETTING UP...** means the
computer has connected but has not yet completed encryption and subscribed to
HID notifications. Actual cursor behavior and reconnection require testing with
your computer; a successful build alone does not verify Bluetooth compatibility.

## Build and upload

Uses ESP32 core **3.3.12**, its built-in NimBLE stack, and **GFX Library for
Arduino 1.6.9**. No extra Bluetooth library is required.

From this directory:

```bash
arduino-cli compile --profile esp32 .
arduino-cli upload --profile esp32 --port /dev/esp32 .
```

Change the port if necessary. Uploading replaces the app currently on the board.
The profile enables USB serial, PSRAM, and a 3 MB app partition.

Display: CO5300 QSPI, CS 46, SCK 10, D0–D3 11–14, reset 21, column offset 20.
Touch: FT3168 at I²C `0x38`, SDA 47 / SCL 48.

Touch polling targets roughly 8 ms between iterations; BLE requests a 7.5–15 ms
connection interval, which the computer may override. The screen is static while
moving, so full-screen redraws do not compete with input. Connection changes
clear held buttons and require finger lift before accepting new gestures.
Touch bus failures longer than 150 ms release held buttons as a safety measure.

## Host tests

```bash
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/gestures_test.cpp -o /tmp/opencode/trackpad_gestures_test
/tmp/opencode/trackpad_gestures_test
```

Tests cover relative motion, taps, long presses, button mapping, drag latch,
scroll accumulation/direction, region exits, jump rejection, reset, and timer
rollover. They do not emulate BLE or the physical touchscreen.
