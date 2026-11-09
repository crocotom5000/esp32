# Bluetooth Mouse

A mouse-controlled pixel drawing app on the **Waveshare ESP32-S3-Touch-AMOLED-1.64**.
The board acts as a **Bluetooth LE HID host**, not as a mouse for a computer.

- Small cyan arrow on a dark, dotted canvas; movement is clamped to the canvas.
- Left-click paints one white pixel at the cursor's tip. Hold left and move to
  draw continuous, one-pixel-wide strokes. Moving without left held does not paint.
- **Wheel down** smears all ink downward; **wheel up** smears it upward, like
  dragging a finger through wet paint. Each notch pulls the strongest ink four
  pixels, leaving a fading trail. Reversing direction pulls the ink back upward
  or downward; it is not an undo. Ink dragged off the canvas is clipped.
- Paint stays underneath the cursor and survives pairing/reconnection screens.
  Drawings are kept in RAM only: **Clear** or restarting the board erases them.
- Live left/middle/right button indicators and wheel count.
- Touchscreen pairing list, with names, addresses, signal strength, and up to
  eight nearby HID devices across two pages. You explicitly select the device.
- Remembers the selected mouse and its Bluetooth bond across reboots. Reconnects
  when the mouse wakes; handles rotating private addresses when the mouse shares
  its identity key. Never reconnects to an unrelated mouse just by name.
- Tries the saved identity directly on startup and after disconnection, as well
  as scanning. Bonded mice may use directed reconnect advertising rather than
  appearing in an ordinary discovery scan.
- Bluetooth scanning/connecting runs separately from display and touch updates.
- Cursor updates target **60 Hz** using small changed rectangles, rather than
  redrawing and uploading the entire screen on every movement. No transfers are
  needed while idle. Large smears/clears and screen changes can take longer.

## Pair your mouse

1. Select **Bluetooth / BLE** on the mouse, not its USB-dongle mode. Choose a
   spare device slot if it has several, and hold its pairing button until its
   LED flashes. Disconnect it from a computer/phone that might claim it first.
2. On first boot, the board opens **PAIR A MOUSE** and scans automatically.
3. Tap your mouse's name. Wait for the cursor screen, then move the mouse.
4. Later, simply wake the same mouse; the board reconnects automatically.

Normal restarts and sketch uploads preserve the saved mouse and Bluetooth keys.
You do **not** need to put the mouse back in pairing mode after updating the app:
leave it in the same device slot and move/click to wake it. Connection attempts
repeat while it is asleep. Keep **Erase All Flash Before Sketch Upload** disabled
and the partition layout unchanged; erasing NVS/flash removes the keys and does
require pairing again.

Touch controls:

- **Pair** opens pairing mode and disconnects the current mouse.
- **Rescan** refreshes the list; **Next page** shows more devices when available.
- **Cancel** exits pairing and reconnects the saved mouse.
- **Forget saved mouse** removes this app's saved device and that device's bond,
  then returns to pairing. It does not erase other Bluetooth bonds.
- **Center** recenters the pointer and resets the wheel counter.
- **Clear** erases the drawing without changing the pointer or mouse pairing.

Scan cycles take three seconds. Touch requests during a scan or connection are
processed when that operation finishes (connection timeout: eight seconds).
Security negotiation has a ten-second deadline and can be cancelled. An idle
mouse is not disconnected just because it sends no movement reports.

## Compatibility / troubleshooting

**ESP32-S3 supports BLE only.** Bluetooth Classic-only mice, proprietary 2.4 GHz
USB receivers, and wired USB mice will not work with this app. This is not a
phone/PC pairing screen: put the *mouse* into pairing mode and select it on the
board.

The app uses the standard HID service (`0x1812`), parses the HID Report Map, and
uses Report Reference descriptors to select mouse input notifications. It
supports signed relative X/Y fields up to 16 bits (including packed 12-bit
reports), five buttons, and an optional wheel in the same report. A fixed Boot
Mouse protocol fallback is available on mice that expose it. Proprietary
protocols, absolute pointing devices, and split movement/button reports are not
fully supported. Nearby keyboards can appear in the HID list, but are not
accepted as mice.

Pairing uses encrypted **Just Works** bonding: no PIN is needed, but this does
not provide man-in-the-middle authentication. Mice requiring PIN entry are not
supported. If pairing fails after changing the mouse's device slot or clearing
its pairing, use **Pair → Forget saved mouse**, put the mouse back in pairing
mode, then select it again. Re-pairing may also be needed if a mouse changes its
address without sharing an identity key.

Serial output at **115200 baud** lists restored mouse identity/bond count, storage
results, HID candidates, connection attempts, authentication results, and the
selected report layout. Send **`?`** to print the current saved/connection status,
display updates/second, mouse reports/second, render/transfer timing, and actual
BLE connection interval. Idle FPS is intentionally zero. Send **`b`** for a brief
full-screen-versus-small-cursor renderer benchmark; it animates only a test cursor,
without painting or changing the real mouse position or pairing. Private keys are
never printed. A connection is only shown as ready after encryption and
notification subscription succeed.

## Rendering and effects

The original always-full redraw measured about 20 FPS on the board: approximately
13.5 ms composing and 33.5 ms sending each frame. The fast path restores only the
old cursor, new cursor, changed ink, and any changed footer. Transfers use packed
crops with the correct framebuffer row stride, so moving a small cursor does not
require a 255 KB screen upload. Restored regions and all transfer chunks are
expanded to the CO5300's required two-pixel column/row alignment, including odd
cursor positions, so the old cursor is fully erased. The display checks for changes at up to 60 Hz;
actual motion smoothness also depends on the mouse's report rate. The host asks
for a 7.5–15 ms BLE connection interval as a best-effort latency improvement.

Ink uses one byte of density per canvas pixel so smears can fade instead of
stretching solid white lines. Scroll work is coalesced/bounded per frame. BLE
callbacks enqueue input; canvas painting, smearing, and display work run on the
UI task, outside interrupt-disabled critical sections. The saved pairing format
is unchanged.

## Build and upload

Uses ESP32 core **3.3.12** (its built-in NimBLE stack) and **GFX Library for
Arduino 1.6.9**. No additional Bluetooth library is required. The profile enables
USB serial, PSRAM, and a 3 MB application partition.

From this directory:

```bash
arduino-cli compile --profile esp32 .
arduino-cli upload --profile esp32 --port /dev/esp32 .
```

Change the port if needed. Uploading replaces the sketch currently on the board.
Display/touch wiring is the same as the other apps: CO5300 QSPI on CS 46, SCK 10,
D0–D3 11–14, RST 21, column offset 20; FT3168 touch on I²C SDA 47 / SCL 48,
address `0x38`.

## Host tests

Tests cover report IDs, padding, packed and wide signed axes, button/wheel
decoding, composite devices, global Push/Pop, malformed descriptors, randomized
inputs, cursor bounds, and wheel overflow:

```bash
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/mouse_hid_test.cpp -o /tmp/opencode/mouse_hid_test
/tmp/opencode/mouse_hid_test
```

Drawing tests cover single pixels, quick clicks between frames, released/right
buttons, continuous strokes in every direction, canvas bounds, clearing, and
independent paint snapshots:

```bash
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/drawing_test.cpp -o /tmp/opencode/mouse_drawing_test
/tmp/opencode/mouse_drawing_test
```

Smear and renderer tests cover wheel direction, fading trails, whole-canvas ink,
edge clipping, dirty bounds, overlapping/distant cursor rectangles, and packed
crop row strides:

```bash
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/smear_test.cpp -o /tmp/opencode/mouse_smear_test
/tmp/opencode/mouse_smear_test
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/render_regions_test.cpp -o /tmp/opencode/mouse_render_regions_test
/tmp/opencode/mouse_render_regions_test
```

Pairing-storage tests check compatibility with the original on-flash record,
identity-based reconnect targets, rotating addresses without bond deletion,
unchanged-record detection, and invalid-record handling:

```bash
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/peer_store_test.cpp -o /tmp/opencode/mouse_peer_store_test
/tmp/opencode/mouse_peer_store_test
```

The private-address tests use OpenSSL development headers and `libcrypto` to
check AES byte order against the Bluetooth Core specification's `ah()` vector:

```bash
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/mouse_privacy_test.cpp -lcrypto -o /tmp/opencode/mouse_privacy_test
/tmp/opencode/mouse_privacy_test
```
