# Bouncing Balls

Seven colorful balls move around the **Waveshare ESP32-S3-Touch-AMOLED-1.64**
screen, bouncing off the edges and each other.

- Equal-mass elastic collisions, with no continuous gravity or friction.
- Move the board to nudge the balls; shake it to scatter them, including shakes
  toward/away from the screen. Added motion is speed-limited to stay controllable.
- The onboard QMI8658 accelerometer filters out steady gravity and small noise,
  so the balls resume free bouncing when the board is still.
- Border turns amber while movement is detected; red means IMU data is unavailable
  (ordinary bouncing still works).
- Random starting directions and speeds on each boot.
- Shaded balls on a black background, composed off-screen to avoid clearing flicker.
- Targets 30 fps, with independent 240 Hz physics steps.
- Runs automatically; no touch controls or Wi-Fi required.

## Build and flash

From this directory:

```bash
arduino-cli compile --profile esp32 .
arduino-cli upload --profile esp32 --port /dev/esp32 .
```

Change the port if needed. The profile pins ESP32 core 3.3.12, GFX Library
for Arduino 1.6.9, and QMI8658 1.0.1, matching the other sketches in this repository.
The IMU uses I²C on SDA 47 / SCL 48, with its axes mapped to the portrait display.

## Tuning

In `BouncingBalls.ino`, change `BALL_RADIUS` or the starting speed range in
`createBalls()`. Keep the radius below 40 pixels so the starting grid does not
overlap, and use modest speeds so balls cannot pass through each other between
physics steps. To change `BALL_COUNT`, also adjust the palette and starting grid.

In `motion.h`, tune the 0.30 s gravity-tracking time, 0.045 g movement deadzone,
0.18 g out-of-plane shake deadzone, force multipliers, or 320 px/s motion speed cap.
The accelerometer is polled up to 100 Hz between frames (display transfers can
reduce that rate); reactions are automatic, not hardware interrupt-driven.

## Tests (no board needed)

```bash
g++ -std=c++11 -Wall -Wextra -pedantic tests/physics_test.cpp -o /tmp/opencode/bouncing-balls-test
/tmp/opencode/bouncing-balls-test
g++ -std=c++11 -Wall -Wextra -pedantic tests/motion_test.cpp -o /tmp/opencode/bouncing-balls-motion-test
/tmp/opencode/bouncing-balls-motion-test
```
