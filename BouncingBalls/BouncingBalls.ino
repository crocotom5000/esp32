// Bouncing balls for Waveshare ESP32-S3-Touch-AMOLED-1.64.
// CO5300 AMOLED, 280x456, QSPI, 20 px column offset.
// Compose each frame off-screen to avoid visible clearing/flicker.
#define ESP32QSPI_MAX_PIXELS_AT_ONCE 8192
#include <Arduino_GFX_Library.h>
#include <QMI8658.h>
#include <Wire.h>
#include <esp_random.h>

#include "motion.h"

#define TFT_CS     46
#define TFT_SCK    10
#define TFT_D0     11
#define TFT_D1     12
#define TFT_D2     13
#define TFT_D3     14
#define TFT_RST    21
#define TFT_WIDTH  280
#define TFT_HEIGHT 456
#define IMU_SDA    47
#define IMU_SCL    48

using namespace bouncingballs;

static const uint8_t BALL_COUNT = 7;
static const float BALL_RADIUS = 16.0f;
static const float PHYSICS_DT = 1.0f / 240.0f;
static const uint32_t FRAME_US = 33333; // target 30 fps
static const Bounds ARENA = {1.0f, 1.0f, TFT_WIDTH - 2.0f, TFT_HEIGHT - 2.0f};

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    TFT_CS, TFT_SCK, TFT_D0, TFT_D1, TFT_D2, TFT_D3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, TFT_RST, 0 /* rotation */, TFT_WIDTH, TFT_HEIGHT,
    20 /* col offset */, 0 /* row offset */);
Arduino_Canvas *frame = nullptr;
Ball balls[BALL_COUNT];
uint32_t previousUs = 0;
float accumulator = 0.0f;
QMI8658 imu;
bool imuReady = false;
MotionFilter motionFilter;
MotionForce motionForce = {0, 0, 0};
uint32_t lastPollUs = 0;
uint32_t lastGoodSampleUs = 0;

static const uint16_t COLORS[BALL_COUNT] = {
    RGB565(70, 200, 255), RGB565(255, 95, 120), RGB565(255, 200, 65),
    RGB565(100, 235, 160), RGB565(175, 125, 255), RGB565(255, 145, 65),
    RGB565(70, 225, 215)};

void createBalls()
{
  // A spaced-out grid guarantees a non-overlapping start, with random headings.
  for (uint8_t i = 0; i < BALL_COUNT; i++)
  {
    const float angle = random(6284) / 1000.0f;
    const float speed = random(85, 131);
    balls[i] = {60.0f + (i % 3) * 80.0f, 90.0f + (i / 3) * 135.0f,
                cosf(angle) * speed, sinf(angle) * speed, BALL_RADIUS};
  }
}

void initMotion()
{
  imuReady = imu.begin(IMU_SDA, IMU_SCL);
  if (!imuReady)
  {
    Serial.println("IMU not found; continuing with ordinary bouncing balls");
    return;
  }
  Wire.setClock(400000);
  Wire.setTimeOut(10);
  imu.setAccelRange(QMI8658_ACCEL_RANGE_4G);
  imu.setAccelODR(QMI8658_ACCEL_ODR_250HZ);
  imu.setAccelUnit_mg(true);
  imu.enableSensors(QMI8658_ENABLE_ACCEL);
  delay(200);
}

void sampleMotion(uint32_t now)
{
  if (!imuReady || now - lastPollUs < 10000) return;
  lastPollUs = now;
  const uint32_t elapsedUs = now - lastGoodSampleUs;
  if (elapsedUs > 100000)
  {
    // Never keep accelerating from a stale sample if the sensor stops responding.
    motionForce = {0, 0, 0};
    motionFilter = MotionFilter();
  }
  QMI8658_Data data;
  if (!imu.readSensorData(data) || !isfinite(data.accelX) ||
      !isfinite(data.accelY) || !isfinite(data.accelZ)) return;
  motionForce = motionFilter.update(data.accelX, data.accelY, data.accelZ,
                                    (elapsedUs > 100000 ? 10000 : elapsedUs) / 1000000.0f);
  lastGoodSampleUs = now;
}

void drawFrame()
{
  memset(frame->getFramebuffer(), 0, (size_t)TFT_WIDTH * TFT_HEIGHT * 2);
  const bool sensorOK = imuReady && micros() - lastGoodSampleUs <= 100000;
  const uint16_t border = !sensorOK ? RGB565(140, 40, 40) :
      (hasMotion(motionForce) ? RGB565(255, 180, 60) : RGB565(30, 40, 55));
  frame->drawRect(0, 0, TFT_WIDTH, TFT_HEIGHT, border);
  for (uint8_t i = 0; i < BALL_COUNT; i++)
  {
    const int16_t x = lroundf(balls[i].x);
    const int16_t y = lroundf(balls[i].y);
    const int16_t radius = lroundf(balls[i].radius);
    const uint16_t color = COLORS[i];
    // A darker rim and small highlight give each ball a little depth.
    const uint16_t shade = (color >> 1) & 0x7BEF;
    frame->fillCircle(x, y, radius, shade);
    frame->fillCircle(x - 1, y - 2, radius - 2, color);
    frame->fillCircle(x - 5, y - 6, 3, RGB565(235, 250, 255));
  }
  gfx->draw16bitRGBBitmap(0, 0, frame->getFramebuffer(), TFT_WIDTH, TFT_HEIGHT);
}

void setup()
{
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);
#endif
  if (!gfx->begin())
  {
    Serial.println("Display init failed");
    while (true) delay(1000);
  }
  gfx->setBrightness(200);
  gfx->fillScreen(RGB565_BLACK);

  frame = new Arduino_Canvas(TFT_WIDTH, TFT_HEIGHT, nullptr);
  if (!frame || !frame->begin() || !frame->getFramebuffer())
  {
    Serial.println("Frame buffer allocation failed");
    while (true) delay(1000);
  }

  randomSeed(esp_random());
  createBalls();
  initMotion();
  sampleMotion(micros());
  drawFrame();
  previousUs = micros();
}

void loop()
{
  const uint32_t now = micros();
  sampleMotion(now);
  const uint32_t elapsedUs = now - previousUs; // wrap-safe
  if (elapsedUs < FRAME_US)
  {
    delay(1);
    return;
  }
  previousUs = now;

  // Fixed small steps prevent tunneling at normal speeds, independently of
  // display refresh speed. Cap catch-up after a long stall to stay responsive.
  accumulator += (elapsedUs > 100000 ? 100000 : elapsedUs) / 1000000.0f;
  while (accumulator >= PHYSICS_DT)
  {
    applyMotion(balls, BALL_COUNT, motionForce, PHYSICS_DT);
    step(balls, BALL_COUNT, ARENA, PHYSICS_DT);
    accumulator -= PHYSICS_DT;
  }
  drawFrame();
}
