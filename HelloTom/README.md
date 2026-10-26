# HelloTom

Displays a two-screen UI on a **Waveshare ESP32-S3-Touch-AMOLED-1.64** using the Arduino ESP32 core and the Arduino GFX library.

- Screen 1: **Hello Tom**
- Screen 2: **how are you?**

Swipe **left** to slide to screen 2, swipe **right** to slide back.

## Board / toolchain

| Item | Value |
| --- | --- |
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.64 (280x456 AMOLED) |
| Arduino core | `esp32:esp32` 3.3.12 |
| FQBN | `esp32:esp32:waveshare_esp32_s3_touch_amoled_164` |
| Upload port | `/dev/ttyACM0` |

## Required libraries

| Library | Version used | Purpose | Source |
| --- | --- | --- | --- |
| **GFX Library for Arduino** | 1.6.9 | QSPI bus + CO5300 AMOLED driver, text rendering | Arduino Library Manager (`moononournation`) |

Install with:

```bash
arduino-cli lib install "GFX Library for Arduino@1.6.9"
```

This is the only library the sketch needs. It is also pinned in `sketch.yaml`, so
`arduino-cli compile --profile esp32 .` will install/use that version automatically.

### Not used (safe to remove)

These were installed while probing drivers and are **not** referenced by the sketch:

```bash
arduino-cli lib uninstall "LovyanGFX" "Adafruit GFX Library" "Adafruit BusIO"
```

## Display wiring (as used in the sketch)

| Signal | GPIO |
| --- | --- |
| QSPI CS | 46 |
| QSPI SCK | 10 |
| QSPI D0 | 11 |
| QSPI D1 | 12 |
| QSPI D2 | 13 |
| QSPI D3 | 14 |
| Panel reset | 21 |
| Panel driver | CO5300 |
| Resolution | 280 x 456 |
| Column offset | 20 (panel visible area starts at x=20 in controller RAM) |
| Brightness | command `0x51`, 0 = off, 255 = max |

Touch controller: FT3168 on I2C address `0x38`, SDA 47, SCL 48 (I2C at 400 kHz).

| Touch register | Meaning |
| --- | --- |
| `0x00` | mode (write `0x00` for normal mode during init) |
| `0x02` | number of active touches (0 = released) |
| `0x03` / `0x04` | touch X high nibble / low byte |
| `0x05` / `0x06` | touch Y high nibble / low byte |

Touch coordinates map directly to the display axes (no swap). The Waveshare
LVGL demo swaps X/Y only because it rotates the display by 270 degrees.

## Gestures and animation

- A swipe is recognized on finger release when it moved at least 60 px
  horizontally, mostly horizontally (|dx| >= 1.5*|dy|), and took under 1 s.
- Swipe left goes forward, swipe right goes back.
- Both screens are black with centered white text, so the slide animates only a
  280x64 strip around the text (two `Arduino_Canvas` buffers in internal RAM).
  Everything outside the strip is already black, which makes the strip motion
  look like a full-screen slide without needing PSRAM or 500 KB of framebuffers.
- Swipe details are printed to `Serial` (visible over USB with
  `CDCOnBoot=cdc`) for debugging.

## Build and flash

```bash
cd /home/tom/esp32/HelloTom
arduino-cli compile --profile esp32 .
arduino-cli upload -p /dev/ttyACM0 --profile esp32 .
```

To build with USB serial (needed for the touch debug logs):

```bash
arduino-cli compile --fqbn esp32:esp32:waveshare_esp32_s3_touch_amoled_164:CDCOnBoot=cdc .
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:waveshare_esp32_s3_touch_amoled_164:CDCOnBoot=cdc .
```

Notes:

- `Serial` is routed to UART0 by default on this board. Add
  `--board-options CDCOnBoot=cdc` to both compile and upload if you want serial
  logs over USB.
- If upload fails with a permission error, run `sudo chmod 666 /dev/ttyACM0`
  or add your user to the `uucp` group.
- The board has a single USB-C port. A **charge-only** USB-A-to-C cable will
  power the board but never enumerate: `lsusb` shows nothing and no
  `/dev/ttyACM*` appears. Use a data-capable cable for flashing.
- If the board was previously on another port it may come up as `/dev/ttyACM1`
  instead of `/dev/ttyACM0`; check `arduino-cli board list`.
