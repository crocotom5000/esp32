# 2048 (ESP32-S3-Touch-AMOLED-1.64)

A 2048 game for the **Waveshare ESP32-S3-Touch-AMOLED-1.64** (280x456 AMOLED).

The usual 4x4 board is replaced by a **4 columns x 5 rows** board to use the
portrait screen better. The header shows the tile as **2^11**, because that is
what 2048 is.

- **Swipe** left/right/up/down to move and merge tiles.
- A swipe that cannot move anything (tiles already against that wall) gives a
  small board bump in that direction, so the touch still feels registered.
- Equal tiles merge; every move spawns a new 2 (90%) or 4 (10%).
- Reaching **2048** shows a "YOU WIN!" panel once; tap to keep playing.
- No moves left shows "GAME OVER"; tap to start a new game.
- Score is shown live and the **best score is saved in NVS** (survives reboots).

## Board / toolchain

| Item | Value |
| --- | --- |
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.64 |
| Arduino core | `esp32:esp32` 3.3.12 |
| FQBN | `esp32:esp32:waveshare_esp32_s3_touch_amoled_164` |
| Upload port | `/dev/ttyACM0` |
| Display | CO5300 AMOLED, 280x456, QSPI, column offset 20 |
| Touch | FT3168, I2C address `0x38`, SDA 47, SCL 48 |

## Required library

| Library | Version | Purpose |
| --- | --- | --- |
| **GFX Library for Arduino** | 1.6.9 | QSPI bus, CO5300 driver, off-screen canvas |

```bash
arduino-cli lib install "GFX Library for Arduino@1.6.9"
```

It is pinned in `sketch.yaml`, so `arduino-cli compile --profile esp32 .`
installs/uses it automatically. No other libraries are needed (Wire and
Preferences ship with the ESP32 core).

## Build and flash

```bash
cd /home/tom/esp32/Game2048
arduino-cli compile --profile esp32 .
arduino-cli upload -p /dev/ttyACM0 --profile esp32 .
```

For USB serial logs (move/score debug output), build with CDC enabled:

```bash
arduino-cli compile --fqbn esp32:esp32:waveshare_esp32_s3_touch_amoled_164:CDCOnBoot=cdc .
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:waveshare_esp32_s3_touch_amoled_164:CDCOnBoot=cdc .
```

Notes:

- The board has a single USB-C port; a charge-only USB-A-to-C cable powers it but
  never enumerates (no `/dev/ttyACM*`). Use a data-capable cable.
- If the port is not writable: `sudo chmod 666 /dev/ttyACM0` or join `uucp`.
- The device may appear as `/dev/ttyACM1` after switching ports; check
  `arduino-cli board list`.

## Implementation notes

- `game_logic.h` is pure C++ (no Arduino dependencies): board move/merge,
  move list for animation, `hasMoves()`, `maxTile()`. It is unit-tested on the
  host (`/tmp/opencode/test_game.cpp` during development).
- The board plus a 6 px margin on every side (280x348) is composed in an
  off-screen `Arduino_Canvas` framebuffer (~195 KB in internal RAM) and pushed
  with **one** DMA transfer per frame. Drawing slots and tiles straight to the
  panel made each frame show a half-cleared board, which read as flicker; a
  single blit fixes that. Using the full screen width also means no clipped
  blits and no stale edge strips.
- Compose writes go directly into the framebuffer as 32-bit fills
  (`fbFill`/`fbRect`/`fbRoundRect`). The library's per-pixel primitives cost
  ~10 ms per frame and made moves drag; the fills take ~1.5 ms.
- A move animates in 3 frames (ease-out): two sliding frames, then a final
  frame that paints the settled board (merged values plus the spawned tile).
  That last frame replaces the separate full repaint, so one move is 3 blits at
  ~23 ms each, about 85 ms total on this QSPI panel.
- The animation is interruptible: if the next swipe is already under way (or has
  been released) while a move is sliding, the board settles immediately and the
  new move starts without waiting for the remaining frames. This is what keeps
  consecutive swipes from feeling hesitant; a gesture that arrives mid-animation
  is never dropped.
- Because each frame is one coherent image, the 4 px gutters need no explicit
  clearing: sliding tiles cannot leave trails between cells.
- A blocked swipe shifts the whole board image 6 px toward the swipe inside the
  buffer, holds it for up to 140 ms, then re-blits at rest. The 6 px margin means
  the shift never clips tiles or leaves stale pixels. The hold is cut short if
  the next swipe starts, so the bump never swallows a gesture.
- SPI chunks are raised to 8192 pixels (`ESP32QSPI_MAX_PIXELS_AT_ONCE`) before
  including the library; the panel tops out around 8-10 MB/s for bitmap writes
  regardless, so this is a small win, not a fix.
- Touch is sampled on every loop tick *and* inside each animation frame. A
  release that happens mid-animation is stored as a delta and applied as soon
  as the slide finishes, so rapid consecutive swipes chain without a pause.
  Without this, a swipe started during a move was only seen after the animation
  ended, so its measured distance was short and it could be dropped.
- Debug console over USB serial (115200): `1`-`4` move left/right/up/down,
  `b` trigger the blocked-swipe bump, `h` print free heap.
- Layout: 64 px cells, 4 px gaps, board at x=6, y=84; score box top-right;
  hint at the bottom.
