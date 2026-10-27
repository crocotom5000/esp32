// Spirit Level for Waveshare ESP32-S3-Touch-AMOLED-1.64
//
// A bullseye bubble level driven by the onboard QMI8658 accelerometer.
// - Bubble moves toward the raised side of the board.
// - Turns green when the board is level (within ~1.5 degrees).
// - Tap anywhere to tare (treat the current attitude as level).
//
// The IMU has no magnetometer, so this senses tilt, not magnetic north.

#include <Arduino_GFX_Library.h>
#include <QMI8658.h>
#include <Wire.h>

#define TFT_CS     46
#define TFT_SCK    10
#define TFT_D0     11
#define TFT_D1     12
#define TFT_D2     13
#define TFT_D3     14
#define TFT_RST    21

#define TFT_WIDTH  280
#define TFT_HEIGHT 456

#define TOUCH_SDA  47
#define TOUCH_SCL  48
#define TOUCH_ADDR 0x38

#define IMU_SDA    47
#define IMU_SCL    48

#define LEVEL_SIZE 216
#define LEVEL_X    32
#define LEVEL_Y    118
#define CX         108
#define CY         108
#define RADIUS     100
#define BUBBLE_R   17

// Flip these if the bubble moves the wrong way on your board.
// FLIP_X: east/west bubble direction. FLIP_Y: north/south bubble direction.
#define FLIP_X 1
#define FLIP_Y 0

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    TFT_CS, TFT_SCK, TFT_D0, TFT_D1, TFT_D2, TFT_D3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, TFT_RST, 0, TFT_WIDTH, TFT_HEIGHT,
    20 /* col offset */, 0 /* row offset */);

Arduino_Canvas *level = new Arduino_Canvas(LEVEL_SIZE, LEVEL_SIZE, nullptr);
QMI8658 imu;

// Static background of the level dial. Only the old/new bubble area is
// restored and pushed each frame, so the display isn't redrawn 100% per frame.
uint16_t *levelBg = nullptr;
int lastBx = -1000;  // forces the first frame to draw the bubble
int lastBy = -1000;

// Smoothed acceleration (mg) and tare offset.
float ax = 0, ay = 0, az = 0;
float ax0 = 0, ay0 = 0;
bool tared = false;
uint32_t taredAt = 0;

bool autoTarePending = true;
uint32_t bootAt = 0;

bool touchWasDown = false;

// Text is throttled separately from the bubble for a smoother, faster loop.
uint32_t lastStatusMs = 0;
uint32_t lastPrintMs = 0;
uint32_t frames = 0;

const uint16_t C_BG      = RGB565_BLACK;
const uint16_t C_RING    = RGB565(70, 70, 70);
const uint16_t C_RING2   = RGB565(45, 45, 45);
const uint16_t C_GRID    = RGB565(60, 60, 60);
const uint16_t C_TEXT    = RGB565(225, 225, 225);
const uint16_t C_DIM     = RGB565(140, 140, 140);
const uint16_t C_GREEN   = RGB565(0, 220, 110);
const uint16_t C_AMBER   = RGB565(255, 175, 40);
const uint16_t C_RED     = RGB565(255, 70, 70);
const uint16_t C_CYAN    = RGB565(80, 220, 255);

// ---------------------------------------------------------------------------
// Touch (FT3168, same I2C bus as the IMU)
// ---------------------------------------------------------------------------

bool i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool touchRead(uint16_t &x, uint16_t &y) {
  uint8_t count = 0;
  if (!i2cRead(TOUCH_ADDR, 0x02, &count, 1) || count == 0) return false;
  uint8_t b[4];
  if (!i2cRead(TOUCH_ADDR, 0x03, b, 4)) return false;
  x = ((b[0] & 0x0F) << 8) | b[1];
  y = ((b[2] & 0x0F) << 8) | b[3];
  return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void drawChrome() {
  level->fillScreen(C_BG);

  // Outer and inner rings.
  level->drawCircle(CX, CY, RADIUS, C_RING);
  level->drawCircle(CX, CY, RADIUS - 1, C_RING2);
  level->drawCircle(CX, CY, RADIUS / 2, C_RING2);

  // Crosshair.
  level->drawFastHLine(CX - RADIUS + 5, CY, 2 * RADIUS - 10, C_GRID);
  level->drawFastVLine(CX, CY - RADIUS + 5, 2 * RADIUS - 10, C_GRID);

  // 45-degree tick marks.
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4.0f;
    int x1 = CX + cosf(a) * (RADIUS - 10);
    int y1 = CY + sinf(a) * (RADIUS - 10);
    int x2 = CX + cosf(a) * (RADIUS - 2);
    int y2 = CY + sinf(a) * (RADIUS - 2);
    level->drawLine(x1, y1, x2, y2, C_GRID);
  }

  // Center target.
  level->drawCircle(CX, CY, 12, C_GRID);
  level->drawCircle(CX, CY, 2, C_TEXT);
}

void drawBubble(float dx, float dy, float tiltDeg) {
  uint16_t *fb = level->getFramebuffer();
  if (levelBg == nullptr) {
    drawChrome();
  }

  // A bubble level displaces by tan(tilt), not sin(tilt): the horizontal
  // gravity projection divided by the vertical component. Using tan makes
  // the bubble reach the ring at 45 degrees and track large tilts properly.
  // The QMI8658 is mounted 90 degrees relative to the display on this board:
  // the chip's Y axis runs across the screen (east-west) and the chip's X axis
  // runs up the screen (north-south). So screen X is driven by ay and screen Y
  // by ax. A bubble moves toward the raised (high) side of the board.
  const float span = RADIUS - BUBBLE_R - 4;
  float gz = fabsf(az);
  if (gz < 150.0f) gz = 150.0f;  // near-vertical: pin the bubble at the edge
  float vx = (FLIP_X ? -1.0f : 1.0f) * (ay - ay0) / gz * span;
  float vy = (FLIP_Y ? 1.0f : -1.0f) * (ax - ax0) / gz * span;

  float len = sqrtf(vx * vx + vy * vy);
  if (len > span) {
    vx = vx / len * span;
    vy = vy / len * span;
  }

  int bx = CX + (int)vx;
  int by = CY + (int)vy;

  uint16_t bubbleColor = C_RED;
  if (tiltDeg < 1.5f) bubbleColor = C_GREEN;
  else if (tiltDeg < 5.0f) bubbleColor = C_AMBER;

  if (levelBg != nullptr) {
    // If the bubble is still, don't touch the display at all.
    if (bx == lastBx && by == lastBy) {
      return;
    }

    // Restore a full-width horizontal band covering both the old and new
    // bubble, then draw the bubble and push that band. A band is contiguous
    // in the canvas, so it goes out in one fast blit instead of 200 rows.
    const int pad = BUBBLE_R + 4;
    int y0 = min(lastBy, by) - pad;
    int y1 = max(lastBy, by) + pad;
    if (y0 < 0) y0 = 0;
    if (y1 > LEVEL_SIZE - 1) y1 = LEVEL_SIZE - 1;
    const int rows = y1 - y0 + 1;

    for (int y = y0; y <= y1; y++) {
      memcpy(fb + y * LEVEL_SIZE, levelBg + y * LEVEL_SIZE,
             LEVEL_SIZE * sizeof(uint16_t));
    }

    level->fillCircle(bx + 2, by + 3, BUBBLE_R, RGB565(12, 12, 12));
    level->fillCircle(bx, by, BUBBLE_R, bubbleColor);
    level->drawCircle(bx, by, BUBBLE_R, C_TEXT);
    level->fillCircle(bx - BUBBLE_R / 3, by - BUBBLE_R / 3, BUBBLE_R / 4,
                      RGB565(255, 255, 255));

    gfx->draw16bitRGBBitmap(LEVEL_X, LEVEL_Y + y0,
                            fb + y0 * LEVEL_SIZE, LEVEL_SIZE, rows);
  } else {
    // Fallback if the background buffer could not be allocated.
    level->fillCircle(bx + 2, by + 3, BUBBLE_R, RGB565(12, 12, 12));
    level->fillCircle(bx, by, BUBBLE_R, bubbleColor);
    level->drawCircle(bx, by, BUBBLE_R, C_TEXT);
    level->fillCircle(bx - BUBBLE_R / 3, by - BUBBLE_R / 3, BUBBLE_R / 4,
                      RGB565(255, 255, 255));
    gfx->draw16bitRGBBitmap(LEVEL_X, LEVEL_Y, fb, LEVEL_SIZE, LEVEL_SIZE);
  }

  lastBx = bx;
  lastBy = by;
}

void drawStatus(float tiltDeg, float rollDeg, float pitchDeg) {
  // Status line above the level.
  gfx->fillRect(0, 58, TFT_WIDTH, 40, C_BG);
  gfx->setTextSize(2);
  if (tiltDeg < 1.5f) {
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(20, 64);
    gfx->print("LEVEL");
  } else {
    gfx->setTextColor(C_CYAN);
    gfx->setCursor(20, 64);
    gfx->printf("TILT %.1f deg", tiltDeg);
  }
  if (tared) {
    gfx->setTextColor(C_DIM);
    gfx->setCursor(20, 84);
    gfx->print("tared");
  }

  // Roll / pitch below the level.
  gfx->fillRect(0, 346, TFT_WIDTH, 110, C_BG);
  gfx->setTextSize(2);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(24, 352);
  gfx->printf("ROLL  %+6.1f", rollDeg);
  gfx->setCursor(24, 378);
  gfx->printf("PITCH %+6.1f", pitchDeg);

  gfx->setTextColor(C_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(24, 414);
  gfx->print("tap to tare");
  if (tared) {
    gfx->setCursor(24, 428);
    gfx->print("tap again to re-tare");
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block if no serial monitor is attached
  delay(300);

  gfx->begin();
  gfx->setBrightness(255);
  gfx->fillScreen(C_BG);

  gfx->setTextColor(C_CYAN);
  gfx->setTextSize(3);
  gfx->setCursor(24, 14);
  gfx->print("SPIRIT LEVEL");

  level->begin();
  drawChrome();
  levelBg = (uint16_t *)malloc(LEVEL_SIZE * LEVEL_SIZE * sizeof(uint16_t));
  if (levelBg != nullptr) {
    memcpy(levelBg, level->getFramebuffer(),
           LEVEL_SIZE * LEVEL_SIZE * sizeof(uint16_t));
  }
  gfx->draw16bitRGBBitmap(LEVEL_X, LEVEL_Y, level->getFramebuffer(),
                          LEVEL_SIZE, LEVEL_SIZE);

  Serial.println("IMU init...");
  bool ok = imu.begin(IMU_SDA, IMU_SCL);
  Serial.printf("begin=%d whoami=0x%02X\n", ok, imu.getWhoAmI());

  if (ok) {
    Wire.setClock(400000);  // fast mode: cuts sensor + touch read time
    imu.setAccelRange(QMI8658_ACCEL_RANGE_4G);
    imu.setAccelODR(QMI8658_ACCEL_ODR_250HZ);
    imu.setGyroRange(QMI8658_GYRO_RANGE_512DPS);
    imu.setGyroODR(QMI8658_GYRO_ODR_250HZ);
    imu.setAccelUnit_mg(true);
    imu.setGyroUnit_dps(true);
    imu.enableSensors(QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
    delay(200);
  } else {
    gfx->setTextColor(C_RED);
    gfx->setTextSize(2);
    gfx->setCursor(24, 58);
    gfx->print("IMU not found");
  }

  // Seed the filter so the first frame is stable.
  QMI8658_Data d;
  if (imu.readSensorData(d)) {
    ax = d.accelX;
    ay = d.accelY;
    az = d.accelZ;
  }
  bootAt = millis();
}

void loop() {
  QMI8658_Data d;
  bool ok = imu.readSensorData(d);

  // Time-based low-pass: same real-world response whether the loop runs at
  // 30 fps or 100 fps.
  static uint32_t lastSampleMs = 0;
  uint32_t now = millis();
  float dt = (now - lastSampleMs) / 1000.0f;
  lastSampleMs = now;
  if (dt <= 0.001f || dt > 0.5f) dt = 0.033f;

  if (ok) {
    float k = 1.0f - expf(-dt / 0.035f);  // ~35 ms time constant
    ax = ax * (1.0f - k) + d.accelX * k;
    ay = ay * (1.0f - k) + d.accelY * k;
    az = az * (1.0f - k) + d.accelZ * k;
  }

  // Touch: tap to tare.
  uint16_t tx, ty;
  bool down = touchRead(tx, ty);
  if (down && !touchWasDown) {
    ax0 = ax;
    ay0 = ay;
    tared = true;
    taredAt = millis();
    autoTarePending = false;
    Serial.printf("tare at %.1f %.1f mg\n", ax0, ay0);
  }
  touchWasDown = down;

  float dx = (ax - ax0) / 1000.0f;
  float dy = (ay - ay0) / 1000.0f;
  float z  = fabsf(az) / 1000.0f;
  float tilt = atan2f(sqrtf(dx * dx + dy * dy), z) * 180.0f / PI;

  // If the board boots up sitting roughly flat, treat that attitude as level.
  // This cancels sensor offset and a slightly uneven table. If it boots while
  // being held upright, we leave it alone and wait for a tap.
  if (autoTarePending && millis() - bootAt > 1200 && tilt < 10.0f) {
    ax0 = ax;
    ay0 = ay;
    tared = true;
    taredAt = millis();
    autoTarePending = false;
    Serial.println("auto-tare");
  }

  // Positive roll = east/right edge raised; positive pitch = north/top edge raised.
  float roll  = atan2f(-(ay - ay0), fabsf(az)) * 180.0f / PI;
  float pitch = atan2f(ax - ax0, fabsf(az)) * 180.0f / PI;

  drawBubble(dx, dy, tilt);
  frames++;

  // Text is much slower than the bubble, and doesn't need 60 fps.
  if (now - lastStatusMs >= 80) {
    lastStatusMs = now;
    drawStatus(tilt, roll, pitch);
  }

  if (now - lastPrintMs >= 1000) {
    lastPrintMs = now;
    if (Serial && Serial.availableForWrite() > 100) {
      Serial.printf("ax=%.0f ay=%.0f az=%.0f tilt=%.1f roll=%.1f pitch=%.1f fps=%u\n",
                    ax, ay, az, tilt, roll, pitch, frames);
    }
    frames = 0;
  }

  delay(2);
}
