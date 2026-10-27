# Spirit Level

A bullseye spirit level for the **Waveshare ESP32-S3-Touch-AMOLED-1.64**, driven
by the onboard QMI8658 6-axis IMU.

The QMI8658 has an accelerometer and a gyroscope but **no magnetometer**, so this
app senses tilt, not magnetic north. It is a level, not a compass.

## Features

- Bullseye bubble that moves toward the raised side of the board.
- Physically correct `tan(tilt)` travel: the bubble reaches the ring at 45°.
- Green / amber / red status: **LEVEL** within 1.5°, amber within 5°.
- Roll and pitch readout. Positive roll means the east/right edge is raised;
  positive pitch means the north/top edge is raised.
- **Tap anywhere** to tare the current attitude as level.
- **Auto-zero on boot**: if the board is sitting roughly flat, it tares itself
  after about 1.2 seconds. A slightly uneven table then reads level.
- Smooth 35 ms low-pass filter with fast partial display updates.
- Non-blocking serial diagnostics, so the loop cannot stall when no serial
  monitor is attached.

## Hardware

| Function | Details |
| --- | --- |
| Display | CO5300 AMOLED, 280×456, QSPI |
| Display pins | CS 46, SCK 10, D0 11, D1 12, D2 13, D3 14, RST 21 |
| Display offset | 20 px column offset |
| IMU | QMI8658 at I²C address `0x6B` |
| I²C bus | SDA 47, SCL 48 |
| Touch | FT3168 at I²C address `0x38` (same bus) |

## Build and flash

The sketch is set up as an `arduino-cli` sketch profile in `sketch.yaml`:

- ESP32 core `esp32:esp32` 3.3.12
- `GFX Library for Arduino` 1.6.9
- `QMI8658` 1.0.1 (Lahav Gahali)

Install the libraries if needed:

```bash
arduino-cli lib install "GFX Library for Arduino@1.6.9" "QMI8658@1.0.1"
```

Compile and upload from this directory:

```bash
arduino-cli compile --profile esp32 .
arduino-cli upload --profile esp32 --port /dev/esp32 .
```

Adjust the upload port if your board is not on `/dev/esp32`.

## Controls

| Action | Result |
| --- | --- |
| Tap the screen | Tare the current attitude as level |
| Boot while flat | Auto-tare after ~1.2 s |
| Boot while held upright | No auto-tare; wait for a tap |

## Tuning

At the top of `SpiritLevel.ino`:

- `FLIP_X` / `FLIP_Y` reverse the bubble direction on each axis.
- `BUBBLE_R` and `RADIUS` control the bubble and dial size.

In the loop:

- The filter time constant is `0.035f` (35 ms).
- The auto-tare threshold is 10° and the delay is 1200 ms.

The green/amber thresholds are 1.5° and 5.0° in `drawBubble()` / `drawStatus()`.

## Notes

- The QMI8658 is mounted 90° relative to the display on this board. Screen X is
  driven by `ay` and screen Y by `ax`, which is why the axes are swapped in the
  bubble mapping.
- The display is updated by restoring and pushing only the horizontal band that
  contains both the old and new bubble positions. This avoids sending the whole
  216×216 dial every frame.
- Serial output is guarded and uses `Serial.setTxTimeoutMs(0)`. Without that,
  the USB CDC buffer can fill and block the loop after several seconds.
- A hand-rolled QMI8658 initialization returned all-zero data on this board;
  the `QMI8658` Arduino library works reliably.
