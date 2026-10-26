// Hello Tom - Waveshare ESP32-S3-Touch-AMOLED-1.64
// Display: CO5300 AMOLED, 280x456, QSPI
// Touch:   FT3168 on I2C (SDA 47, SCL 48, addr 0x38)
//
// Screen 1: "Hello Tom"
// Screen 2: "how are you?"
//
// Drag horizontally and the screens follow your finger. On release the
// nearest screen snaps into place (a quick flick also works).
//
// Note: the panel's visible area starts 20 columns into the controller RAM,
// so col_offset1 = 20 is required (same as the Waveshare reference demo).

#include <Arduino_GFX_Library.h>
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

// Set to 1 if the touch panel reports X/Y swapped (Waveshare's LVGL demo
// swaps them, but that demo also rotates the display by 270 degrees).
#define TOUCH_SWAP_XY 0

// Both screens are black with centered white text, so only a horizontal strip
// around the text needs to move. Keeping it small avoids needing PSRAM and
// lets us redraw it at 60+ fps while the finger is down.
#define STRIP_H 64
#define STRIP_Y ((TFT_HEIGHT - STRIP_H) / 2)
#define STRIP_PIXELS ((size_t)TFT_WIDTH * STRIP_H)

#define EDGE_PX        10 // movement needed before picking an axis
#define FLICK_VELOCITY 0.35f // px per ms

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    TFT_CS /* CS */, TFT_SCK /* SCK */,
    TFT_D0 /* D0 */, TFT_D1 /* D1 */, TFT_D2 /* D2 */, TFT_D3 /* D3 */);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, TFT_RST /* RST */, 0 /* rotation */,
    TFT_WIDTH, TFT_HEIGHT,
    20 /* col offset */, 0 /* row offset */);

Arduino_Canvas *screens[2] = {nullptr, nullptr};
uint16_t *composite = nullptr;

// Screen 1 x position in pixels: 0 = screen 1 shown, -TFT_WIDTH = screen 2.
int32_t posPx = 0;

// ---------------------------------------------------------------------------
// Touch (FT3168)
// ---------------------------------------------------------------------------

bool i2cRead(uint8_t reg, uint8_t *buf, uint8_t len)
{
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
  {
    return false;
  }
  if (Wire.requestFrom(TOUCH_ADDR, len) != len)
  {
    return false;
  }
  for (uint8_t i = 0; i < len; i++)
  {
    buf[i] = Wire.read();
  }
  return true;
}

void touchInit()
{
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x00);
  Wire.write(0x00); // normal mode
  Wire.endTransmission();
  delay(50);
}

bool touchRead(uint16_t &x, uint16_t &y)
{
  uint8_t count = 0;
  if (!i2cRead(0x02, &count, 1) || count == 0)
  {
    return false;
  }

  uint8_t buf[4];
  if (!i2cRead(0x03, buf, 4))
  {
    return false;
  }
  x = ((buf[0] & 0x0F) << 8) | buf[1];
  y = ((buf[2] & 0x0F) << 8) | buf[3];
  return true;
}

// ---------------------------------------------------------------------------
// Screens and drawing
// ---------------------------------------------------------------------------

Arduino_Canvas *createScreen(const char *text, uint8_t textSize)
{
  Arduino_Canvas *canvas = new Arduino_Canvas(TFT_WIDTH, STRIP_H, nullptr);
  if (!canvas->begin())
  {
    Serial.println("Canvas allocation failed");
    return nullptr;
  }

  const int16_t textW = strlen(text) * 6 * textSize;
  const int16_t textH = 8 * textSize;

  canvas->fillScreen(RGB565_BLACK);
  canvas->setTextColor(RGB565_WHITE);
  canvas->setTextSize(textSize);
  canvas->setCursor((TFT_WIDTH - textW) / 2, (STRIP_H - textH) / 2);
  canvas->print(text);
  return canvas;
}

// Compose the visible band from both screens at the current offset and send
// it with a single DMA transfer.
void renderAt(int32_t offset)
{
  if (offset > 0)
  {
    offset = 0;
  }
  else if (offset < -TFT_WIDTH)
  {
    offset = -TFT_WIDTH;
  }

  const uint16_t *a = screens[0]->getFramebuffer();
  const uint16_t *b = screens[1]->getFramebuffer();

  // Screen 2 starts at this x coordinate.
  const int32_t split = TFT_WIDTH + offset;

  if (split <= 0)
  {
    memcpy(composite, b, STRIP_PIXELS * 2);
  }
  else if (split >= TFT_WIDTH)
  {
    memcpy(composite, a, STRIP_PIXELS * 2);
  }
  else
  {
    const size_t len1 = (size_t)split * 2;
    const size_t len2 = (size_t)(TFT_WIDTH - split) * 2;
    for (int16_t j = 0; j < STRIP_H; j++)
    {
      const uint16_t *rowA = a + (size_t)j * TFT_WIDTH - offset;
      const uint16_t *rowB = b + (size_t)j * TFT_WIDTH;
      uint16_t *dst = composite + (size_t)j * TFT_WIDTH;
      memcpy(dst, rowA, len1);
      memcpy(dst + split, rowB, len2);
    }
  }

  gfx->draw16bitRGBBitmap(0, STRIP_Y, composite, TFT_WIDTH, STRIP_H);
}

void animateTo(int32_t target)
{
  if (target > 0)
  {
    target = 0;
  }
  else if (target < -TFT_WIDTH)
  {
    target = -TFT_WIDTH;
  }

  const int32_t start = posPx;
  if (start == target)
  {
    renderAt(target);
    return;
  }

  const uint8_t steps = 12;
  for (uint8_t i = 1; i <= steps; i++)
  {
    const float t = (float)i / steps;
    const float eased = 1.0f - (1.0f - t) * (1.0f - t); // ease out
    const int32_t p = start + (int32_t)((target - start) * eased);
    renderAt(p);
    delay(9);
  }

  posPx = target;
  renderAt(target);
}

// ---------------------------------------------------------------------------
// Gestures
// ---------------------------------------------------------------------------

bool touching = false;
bool axisLocked = false;
bool horizontalDrag = false;
uint16_t dragStartX, dragStartY;
uint16_t lastX, lastY;
int32_t dragStartPos = 0;
uint32_t dragStartMs = 0;
uint32_t ignoreUntil = 0;

// Recent samples for release velocity.
uint16_t velPrevX = 0, velLastX = 0;
uint32_t velPrevMs = 0, velLastMs = 0;

void handleTouchMove()
{
  const int16_t rawDx = (int16_t)lastX - (int16_t)dragStartX;
  const int16_t rawDy = (int16_t)lastY - (int16_t)dragStartY;

  if (!axisLocked && (abs((int)rawDx) >= EDGE_PX || abs((int)rawDy) >= EDGE_PX))
  {
    axisLocked = true;
    horizontalDrag = abs((int)rawDx) >= abs((int)rawDy);
  }
  if (axisLocked && !horizontalDrag)
  {
    return; // vertical gesture, leave the screens alone
  }

  const int16_t dragDx = TOUCH_SWAP_XY ? rawDy : rawDx;
  int32_t newPos = dragStartPos + dragDx;
  if (newPos > 0)
  {
    newPos = 0;
  }
  else if (newPos < -TFT_WIDTH)
  {
    newPos = -TFT_WIDTH;
  }

  if (newPos != posPx)
  {
    posPx = newPos;
    renderAt(posPx);

    velPrevX = velLastX;
    velPrevMs = velLastMs;
    velLastX = lastX;
    velLastMs = millis();
  }
}

void handleTouchRelease()
{
  const uint32_t now = millis();
  const uint32_t duration = now - dragStartMs;
  const int16_t rawDx = (int16_t)lastX - (int16_t)dragStartX;
  const int16_t rawDy = (int16_t)lastY - (int16_t)dragStartY;

  // Velocity over the most recent movement, if it is recent enough.
  float velocity = 0.0f;
  if (velLastMs > velPrevMs && (now - velLastMs) < 120)
  {
    velocity = (float)((int16_t)velLastX - (int16_t)velPrevX) /
               (float)(velLastMs - velPrevMs);
  }
  if (TOUCH_SWAP_XY)
  {
    velocity = -velocity; // not expected on this board
  }

  int32_t target;
  if (velocity <= -FLICK_VELOCITY)
  {
    target = -TFT_WIDTH; // flicked left
  }
  else if (velocity >= FLICK_VELOCITY)
  {
    target = 0; // flicked right
  }
  else
  {
    target = (posPx <= -TFT_WIDTH / 2) ? -TFT_WIDTH : 0;
  }

  Serial.printf("drag: dx=%d dy=%d v=%.2f px/ms dur=%lu pos=%ld -> %s\n",
                rawDx, rawDy, velocity, (unsigned long)duration, (long)posPx,
                target == 0 ? "screen 1" : "screen 2");

  if (axisLocked && !horizontalDrag)
  {
    target = (posPx <= -TFT_WIDTH / 2) ? -TFT_WIDTH : 0;
  }

  animateTo(target);
  ignoreUntil = millis() + 150;
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(300);

  if (!gfx->begin())
  {
    Serial.println("Display init failed");
  }
  gfx->setBrightness(255);
  gfx->fillScreen(RGB565_BLACK);

  screens[0] = createScreen("Hello Tom", 4);
  screens[1] = createScreen("how are you?", 3);
  composite = (uint16_t *)aligned_alloc(16, STRIP_PIXELS * 2);

  if (!screens[0] || !screens[1] || !composite)
  {
    Serial.println("Screen buffers not available");
    while (true)
    {
      delay(1000);
    }
  }

  posPx = 0;
  renderAt(posPx);
  touchInit();

  Serial.println("Ready: drag left/right, release to snap");
}

void loop()
{
  uint16_t x, y;
  const bool pressed = touchRead(x, y);
  const uint32_t now = millis();

  if (pressed)
  {
    if (!touching && now >= ignoreUntil)
    {
      touching = true;
      axisLocked = false;
      horizontalDrag = false;
      dragStartX = lastX = velLastX = velPrevX = x;
      dragStartY = lastY = y;
      dragStartPos = posPx;
      dragStartMs = velLastMs = velPrevMs = now;
    }
    else if (touching)
    {
      lastX = x;
      lastY = y;
      handleTouchMove();
    }
  }
  else if (touching)
  {
    touching = false;
    handleTouchRelease();
  }

  delay(4);
}
