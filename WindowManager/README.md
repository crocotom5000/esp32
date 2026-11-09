# Window Manager

A tiny Bluetooth-mouse desktop for the **Waveshare ESP32-S3-Touch-AMOLED-1.64**.
This is a standalone sketch, separate from the BluetoothMouse drawing app.

## Use it

- Starts with **one floating window**, approximately a quarter of the screen
  (`176×164` pixels), containing randomly selected text.
- **Hold left on the title bar and move** to drag a window. The grab point stays
  under the cursor; releasing left drops the window. Windows can extend past the
  **left, right, and bottom** of the screen. The top is a hard stop: title bars
  cannot move above the display. A strip of title bar stays visible at the sides,
  and title bars stay above the bottom dock, so windows can always be retrieved.
- **Click anywhere inside a window** to bring it to the foreground. Clicking
  its body focuses it without dragging. Covered windows do not receive clicks.
- **Click the small + button at the bottom** to create a new window above all
  the others. Each new window has its own stable random text and colored title.
- **Click × at the right of a title bar** to close that window. This does not
  start a drag or click through to a window underneath. Closing the foreground
  window activates the next one behind it.
- The foreground window has a bright border/title. Window shadows and exposed
  portions of the windows underneath are restored as you drag.
- **Pair** at the bottom right works with a mouse click or touchscreen tap.

Up to **12 windows** can be open; the + button dims at the limit and becomes
available again when one is closed. You can close every window and use + to
start again. This version has no resize or application-launching controls.
Window contents, positions, and stacking order are kept in RAM; restarting
returns to one window.

## Mouse pairing

Uses the same saved mouse selection and Bluetooth bond as **BluetoothMouse**.
If your mouse was paired in that app, leave it in the same device slot and
move/click to wake it; there should be no need to pair again when switching
between these sketches with normal uploads.

For a new mouse, click/tap **Pair**, put the mouse into Bluetooth pairing mode,
and tap its name on the display. The pairing screen also provides **Rescan**,
**Next page**, **Cancel**, and **Forget saved mouse**. Forgetting the saved mouse
removes the shared selection and that mouse's bond for both apps, not unrelated
Bluetooth bonds.

Requires a **Bluetooth LE mouse**. ESP32-S3 does not support Classic-only mice,
proprietary USB wireless receivers, or wired USB mice in this app. Pairing uses
encrypted Just Works bonding, without a PIN or MITM authentication. Standard
relative HID report layouts (including packed 12-bit and 16-bit axes) and Boot
Mouse fallback are supported; proprietary/absolute input formats are not.

## Rendering

Cursor updates target 60 Hz and only transfer changed rectangles. A separate
cursor-free scene buffer restores old cursor/window areas without leaving trails.
Layout changes recompose the windows back-to-front; old/new window bounds,
shadows, changed focus borders, and dock changes are included in the damage.
Larger window drags can take longer than small cursor-only updates.
Off-screen window geometry is clipped to the physical display for rendering and
transfers. The dock stays above the windows and owns its clicks, even when a
window's body extends behind it.

Both framebuffers are explicitly allocated in PSRAM. All partial transfers use
the CO5300's required even column/row starts, even sizes, and packed row strides,
including the chunks used for larger rectangles. BLE callbacks queue input;
window management and display work run on the UI task. Quick clicks between
display frames are not lost, and disconnects cancel an active drag.

## Build and upload

Uses ESP32 core **3.3.12**, its built-in NimBLE stack, and **GFX Library for
Arduino 1.6.9**. The sketch is self-contained; its local HID/privacy/storage
helpers are adapted from BluetoothMouse and retain the same saved-record format.

From this directory:

```bash
arduino-cli compile --profile esp32 .
arduino-cli upload --profile esp32 --port /dev/esp32 .
```

Uploading replaces the running sketch, not the other app's source files. Keep
**Erase All Flash Before Sketch Upload** disabled and the partition scheme
unchanged to preserve the mouse bond. Drawings/windows are not saved across an
upload or restart.

Same hardware wiring as the other apps: CO5300 QSPI CS 46, SCK 10, D0–D3 11–14,
RST 21, column offset 20. FT3168 touch uses SDA 47 / SCL 48, I²C address `0x38`.

Serial output at **115200 baud** reports restored pairing and connection progress.
Send **`?`** for mouse status, received reports, window count, foreground window,
drag state, cursor coordinates, and last/max render time. Keys are never printed.

## Host tests

Tests cover title-bar grab offsets, release motion, body-vs-title clicks,
occlusion and click-to-front ordering, the + button and capacity limit,
disconnect cancellation, extreme input, randomized gestures, shadow damage,
panel alignment, crop strides, HID decoding, and shared pairing compatibility.
Close-button tests include occlusion, click-through prevention, activation of
the next window, capacity reuse, closing the last window, and reopening from empty.
Off-screen dragging tests cover overflow/recovery at each allowed edge, the hard
top stop, reachable title bars, dock occlusion, clipped damage, and off-screen close.

```bash
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/window_manager_test.cpp -o /tmp/opencode/window_manager_test
/tmp/opencode/window_manager_test

g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/close_window_test.cpp -o /tmp/opencode/window_close_test
/tmp/opencode/window_close_test

g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/offscreen_window_test.cpp -o /tmp/opencode/window_offscreen_test
/tmp/opencode/window_offscreen_test

g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/mouse_test.cpp -o /tmp/opencode/window_mouse_test
/tmp/opencode/window_mouse_test

g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/render_regions_test.cpp -o /tmp/opencode/window_render_test
/tmp/opencode/window_render_test
```
