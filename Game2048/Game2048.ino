// 2048 for Waveshare ESP32-S3-Touch-AMOLED-1.64
//
// Display: CO5300 AMOLED 280x456 on QSPI
// Touch:   FT3168 on I2C (SDA 47, SCL 48, addr 0x38)
//
// Board: 4 columns x 5 rows (portrait) instead of the usual 4x4.
// Swipe to move; tiles slide and merge. 2048 wins once, then you can keep
// playing. Game over -> tap to restart.
//
// Animation is composed in an off-screen 268x336 buffer and pushed with a
// single DMA transfer per frame, so the display never shows a half-cleared
// board (no flicker/tearing inside a frame).

// Default 1024-pixel SPI chunks spend most of each frame on per-transfer
// overhead. 8192-pixel chunks roughly halve a full-board blit.
#define ESP32QSPI_MAX_PIXELS_AT_ONCE 8192
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <Preferences.h>

#include "game_logic.h"

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

#define SWIPE_MIN_PX 25

using namespace game2048;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    TFT_CS, TFT_SCK, TFT_D0, TFT_D1, TFT_D2, TFT_D3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, TFT_RST, 0 /* rotation */,
    TFT_WIDTH, TFT_HEIGHT,
    20 /* col offset */, 0 /* row offset */);

// ---------------------------------------------------------------------------
// Layout / colors
// ---------------------------------------------------------------------------

static const int16_t CELL = 64;
static const int16_t GAP = 4;
static const int16_t BOARD_X = 6;
static const int16_t BOARD_Y = 84;
static const int16_t BOARD_W = COLS * CELL + (COLS - 1) * GAP; // 268
static const int16_t BOARD_H = ROWS * CELL + (ROWS - 1) * GAP; // 336

// The off-screen buffer covers the full screen width and the board height plus
// 6 px above/below, so a blocked-swipe bump can shift the board without any
// clipping or stale edge strips.
static const int16_t FB_W = TFT_WIDTH;    // 280
static const int16_t FB_H = BOARD_H + 12; // 348
static const int16_t FB_Y = BOARD_Y - 6;  // 78

static const int16_t SCORE_X = 152;
static const int16_t SCORE_Y = 8;
static const int16_t SCORE_W = 118;
static const int16_t SCORE_H = 64;

static const uint16_t COL_BG = RGB565(0, 0, 0);
static const uint16_t COL_SLOT = RGB565(0x1c, 0x1c, 0x1c);
static const uint16_t COL_PANEL = RGB565(0x26, 0x26, 0x26);
static const uint16_t COL_OVERLAY = RGB565(0x12, 0x12, 0x12);
static const uint16_t COL_MUTED = RGB565(0x8a, 0x8a, 0x8a);
static const uint16_t COL_TITLE = RGB565(0xed, 0xc2, 0x2e);
static const uint16_t COL_DARK_TEXT = RGB565(0x77, 0x6e, 0x65);
static const uint16_t COL_LIGHT_TEXT = RGB565(0xf9, 0xf6, 0xf2);

inline int16_t cellX(uint8_t c) { return BOARD_X + c * (CELL + GAP); }
inline int16_t cellY(uint8_t r) { return BOARD_Y + r * (CELL + GAP); }

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------

uint32_t board[ROWS][COLS];
uint32_t score = 0;
uint32_t best = 0;
bool winShown = false;

enum AppState
{
  PLAYING,
  GAME_OVER,
  WIN_SCREEN
};
AppState state = PLAYING;

Preferences prefs;

// Touch gesture state. Shared by the input sampler and by running animations,
// which use it to tell whether the player is already swiping again.
bool touching = false;
uint16_t startX, startY, lastX, lastY;
bool releasePending = false;
int16_t pendingDx = 0, pendingDy = 0;
bool timingOn = false;
uint32_t prevMoveEndMs = 0;  // millis() when the last move finished
uint32_t prevReleaseMs = 0;   // millis() of the last finger release
uint32_t moveFirstMs = 0;     // release -> first animated frame on screen
uint32_t gestureT0 = 0;       // micros() at gesture processing start
uint16_t pendPeakX = 0, pendPeakY = 0; // largest |dx|/|dy| seen while pressed
uint8_t pendSamples = 0;
uint32_t pendDownMs = 0;

// Compact per-gesture trace, kept in RAM so it can be dumped with 'g'.
struct GestureRecord
{
  uint16_t gap;       // ms since the previous release
  int16_t afterMove;  // ms since the previous move finished (negative = during it)
  uint16_t down;      // press duration, ms
  int16_t dx, dy;     // final delta
  uint16_t peakX, peakY;
  uint8_t samples;
  uint8_t dir;        // 0..3, 0xFF = not a swipe
  uint8_t outcome;    // 0 = tap/ignored, 1 = moved, 2 = blocked, 3 = overlay
  uint16_t moveMs;    // whole move, ms
  uint16_t firstMs;   // release -> first frame, ms
};

static const uint8_t GESTURE_LOG = 32;
GestureRecord glog[GESTURE_LOG];
uint8_t glogCount = 0;
uint8_t glogNext = 0;

void logGesture(const GestureRecord &rec)
{
  glog[glogNext] = rec;
  glogNext = (glogNext + 1) % GESTURE_LOG;
  if (glogCount < GESTURE_LOG) glogCount++;

  static const char *DIRS = "LRUD";
  static const char *OUTS[] = {"TAP", "MOVE", "BLOCK", "OVERLAY"};
  char dch = (rec.dir < 4) ? DIRS[rec.dir] : '-';
  Serial.printf("G gap=%u afterMove=%d down=%u d=(%d,%d) peak=(%u,%u) n=%u %c %s move=%u first=%u\n",
                rec.gap, rec.afterMove, rec.down, rec.dx, rec.dy,
                rec.peakX, rec.peakY, rec.samples, dch, OUTS[rec.outcome & 3],
                rec.moveMs, rec.firstMs);
}

bool bumpHeld = false;
uint32_t bumpAt = 0;

uint32_t moveUsLast = 0, moveUsMax = 0;
uint32_t moveUsRing[8] = {0};
uint8_t moveUsPos = 0;

void recordMoveTime(uint32_t us)
{
  moveUsLast = us;
  if (us > moveUsMax) moveUsMax = us;
  moveUsRing[moveUsPos] = us;
  moveUsPos = (moveUsPos + 1) & 7;
}

struct MoveTimer
{
  uint32_t t0;
  MoveTimer() : t0(micros()) {}
  ~MoveTimer() { recordMoveTime(micros() - t0); }
};

// True once the current touch has travelled far enough to count as a swipe.
// Used to cut a running animation short so the next move can start at once.
bool gestureActive()
{
  if (!touching)
  {
    return false;
  }
  const int16_t dx = (int16_t)lastX - (int16_t)startX;
  const int16_t dy = (int16_t)lastY - (int16_t)startY;
  const int16_t adx = (dx < 0) ? -dx : dx;
  const int16_t ady = (dy < 0) ? -dy : dy;
  return (adx >= SWIPE_MIN_PX) || (ady >= SWIPE_MIN_PX);
}

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
// Off-screen board buffer and tile drawing
// ---------------------------------------------------------------------------

Arduino_Canvas *frame = nullptr;
uint16_t *fb = nullptr; // frame->getFramebuffer(), drawn into directly

uint16_t tileColor(uint32_t v)
{
  switch (v)
  {
  case 2: return RGB565(0xee, 0xe4, 0xda);
  case 4: return RGB565(0xed, 0xe0, 0xc8);
  case 8: return RGB565(0xf2, 0xb1, 0x79);
  case 16: return RGB565(0xf5, 0x95, 0x63);
  case 32: return RGB565(0xf6, 0x7c, 0x5f);
  case 64: return RGB565(0xf6, 0x5e, 0x3b);
  case 128: return RGB565(0xed, 0xcf, 0x72);
  case 256: return RGB565(0xed, 0xcc, 0x61);
  case 512: return RGB565(0xed, 0xc8, 0x50);
  case 1024: return RGB565(0xed, 0xc5, 0x3f);
  case 2048: return RGB565(0xed, 0xc2, 0x2e);
  default: return RGB565(0x3c, 0x3a, 0x32);
  }
}

uint16_t tileTextColor(uint32_t v)
{
  return (v <= 4) ? COL_DARK_TEXT : COL_LIGHT_TEXT;
}

uint8_t tileTextSize(const char *s)
{
  const size_t n = strlen(s);
  if (n <= 2) return 4;
  if (n == 3) return 3;
  if (n <= 5) return 2;
  return 1;
}

bool createFrameBuffer()
{
  frame = new Arduino_Canvas(FB_W, FB_H, nullptr);
  if (!frame || !frame->begin())
  {
    return false;
  }
  fb = frame->getFramebuffer();
  return fb != nullptr;
}

// Coordinates inside the frame buffer.
inline int16_t frameX(uint8_t c) { return cellX(c); }
inline int16_t frameY(uint8_t r) { return cellY(r) - FB_Y; }

// The library primitives write one pixel at a time (bounds check per pixel),
// which was slow enough to make moves drag once several tiles were on screen.
// The buffer is plain RGB565, so fill it 32 bits at a time instead.
void fbFill(uint16_t color)
{
  const uint32_t c32 = ((uint32_t)color << 16) | color;
  uint32_t *p = (uint32_t *)fb;
  for (uint32_t i = 0; i < (uint32_t)FB_W * FB_H / 2; i++)
  {
    p[i] = c32;
  }
}

void fbRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > FB_W) w = FB_W - x;
  if (y + h > FB_H) h = FB_H - y;
  if (w <= 0 || h <= 0) return;

  const uint32_t c32 = ((uint32_t)color << 16) | color;
  for (int16_t row = 0; row < h; row++)
  {
    uint16_t *line = fb + (y + row) * FB_W + x;
    if (((x & 1) == 0) && ((w & 1) == 0))
    {
      uint32_t *p = (uint32_t *)line;
      for (int16_t i = 0; i < w / 2; i++)
      {
        p[i] = c32;
      }
    }
    else
    {
      for (int16_t i = 0; i < w; i++)
      {
        line[i] = color;
      }
    }
  }
}

// Horizontal inset per row of a 10 px radius corner (top row first).
static const uint8_t CORNER_INSET[10] = {6, 4, 3, 2, 1, 1, 0, 0, 0, 0};

void fbRoundRect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t color)
{
  const int16_t r = 10;
  fbRect(x + r, y, w - 2 * r, h, color);          // middle block
  fbRect(x, y + r, r, h - 2 * r, color);          // left strip
  fbRect(x + w - r, y + r, r, h - 2 * r, color);  // right strip
  for (int16_t j = 0; j < r; j++)
  {
    const int16_t inset = CORNER_INSET[j];
    fbRect(x + inset, y + j, w - 2 * inset, 1, color);
    fbRect(x + inset, y + h - 1 - j, w - 2 * inset, 1, color);
  }
}

void drawSlotAt(uint8_t r, uint8_t c)
{
  fbRoundRect(frameX(c), frameY(r), CELL, CELL, COL_SLOT);
}

// x/y are frame-buffer coordinates.
void drawTileAt(int16_t x, int16_t y, uint32_t value)
{
  fbRoundRect(x, y, CELL, CELL, tileColor(value));

  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)value);
  const uint8_t size = tileTextSize(buf);
  const int16_t w = strlen(buf) * 6 * size;
  const int16_t h = 8 * size;
  frame->setTextColor(tileTextColor(value));
  frame->setTextSize(size);
  frame->setCursor(x + (CELL - w) / 2, y + (CELL - h) / 2);
  frame->print(buf);
}

void present()
{
  gfx->draw16bitRGBBitmap(0, FB_Y, fb, FB_W, FB_H);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void drawSlots()
{
  for (uint8_t r = 0; r < ROWS; r++)
  {
    for (uint8_t c = 0; c < COLS; c++)
    {
      drawSlotAt(r, c);
    }
  }
}

void drawBoard(int16_t dx = 0, int16_t dy = 0)
{
  fbFill(COL_BG);
  for (uint8_t r = 0; r < ROWS; r++)
  {
    for (uint8_t c = 0; c < COLS; c++)
    {
      fbRoundRect(frameX(c) + dx, frameY(r) + dy, CELL, CELL, COL_SLOT);
      if (board[r][c] != 0)
      {
        drawTileAt(frameX(c) + dx, frameY(r) + dy, board[r][c]);
      }
    }
  }
  present();
}

void drawTitle()
{
  gfx->setTextColor(COL_TITLE);
  gfx->setTextSize(4);
  gfx->setCursor(10, 8);
  gfx->print("2");

  // Superscript "11": 2^11 stores as 2048.
  gfx->setTextSize(2);
  gfx->setCursor(10 + 24, 8);
  gfx->print("11");
}

void drawBest()
{
  char buf[24];
  snprintf(buf, sizeof(buf), "BEST %lu", (unsigned long)best);
  gfx->fillRect(10, 48, 138, 24, COL_BG);
  gfx->setTextColor(COL_MUTED);
  gfx->setTextSize(2);
  gfx->setCursor(10, 52);
  gfx->print(buf);
}

void drawScore()
{
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)score);

  gfx->fillRoundRect(SCORE_X, SCORE_Y, SCORE_W, SCORE_H, 8, COL_PANEL);

  gfx->setTextColor(COL_MUTED);
  gfx->setTextSize(2);
  const int16_t lw = strlen("SCORE") * 12;
  gfx->setCursor(SCORE_X + (SCORE_W - lw) / 2, 14);
  gfx->print("SCORE");

  const uint8_t size = (strlen(buf) <= 6) ? 3 : 2;
  const int16_t vw = strlen(buf) * 6 * size;
  gfx->setTextColor(COL_LIGHT_TEXT);
  gfx->setTextSize(size);
  gfx->setCursor(SCORE_X + (SCORE_W - vw) / 2, (size == 3) ? 34 : 38);
  gfx->print(buf);
}

void drawHint(const char *text)
{
  gfx->fillRect(0, 426, TFT_WIDTH, 30, COL_BG);
  gfx->setTextColor(COL_MUTED);
  gfx->setTextSize(2);
  const int16_t w = strlen(text) * 12;
  gfx->setCursor((TFT_WIDTH - w) / 2, 432);
  gfx->print(text);
}

void drawOverlay(const char *line1, const char *line2, uint16_t accent)
{
  const int16_t w = 232;
  const int16_t h = 148;
  const int16_t x = (TFT_WIDTH - w) / 2;
  const int16_t y = BOARD_Y + (BOARD_H - h) / 2;

  gfx->fillRoundRect(x, y, w, h, 14, COL_OVERLAY);
  gfx->drawRoundRect(x, y, w, h, 14, accent);

  gfx->setTextColor(accent);
  gfx->setTextSize(3);
  const int16_t l1w = strlen(line1) * 18;
  gfx->setCursor(x + (w - l1w) / 2, y + 30);
  gfx->print(line1);

  gfx->setTextColor(COL_LIGHT_TEXT);
  gfx->setTextSize(2);
  const int16_t l2w = strlen(line2) * 12;
  gfx->setCursor(x + (w - l2w) / 2, y + 92);
  gfx->print(line2);
}

// ---------------------------------------------------------------------------
// Game
// ---------------------------------------------------------------------------

void spawnTile()
{
  uint8_t rows[ROWS * COLS];
  uint8_t cols[ROWS * COLS];
  uint8_t n = 0;

  for (uint8_t r = 0; r < ROWS; r++)
  {
    for (uint8_t c = 0; c < COLS; c++)
    {
      if (board[r][c] == 0)
      {
        rows[n] = r;
        cols[n] = c;
        n++;
      }
    }
  }
  if (n == 0)
  {
    return;
  }

  const uint8_t i = random(n);
  board[rows[i]][cols[i]] = (random(10) == 0) ? 4 : 2;
}

void updateBest()
{
  if (score > best)
  {
    best = score;
    drawBest();
  }
}

void animateMove(const MoveResult &res)
{
  // Each step is one full-board blit (~23 ms), so keeping the count low is what
  // makes a move feel snappy. The last frame paints the settled board (merged
  // values plus the newly spawned tile) instead of the sliding tiles, which
  // also removes the extra full repaint that used to follow every move.
  const uint8_t frames = 2;
  uint32_t tCompose = 0;
  uint32_t tBlit = 0;
  uint32_t tTouch = 0;
  uint8_t drawn = 0;

  for (uint8_t f = 1; f <= frames; f++)
  {
    if (f == frames)
    {
      drawBoard();
      break;
    }

    const float t = (float)f / frames;
    const float eased = 1.0f - (1.0f - t) * (1.0f - t); // ease out

    uint32_t t0 = micros();
    fbFill(COL_BG);
    drawSlots();
    for (uint8_t i = 0; i < res.moveCount; i++)
    {
      const TileMove &m = res.moves[i];
      const int16_t x0 = frameX(m.fromC);
      const int16_t y0 = frameY(m.fromR);
      const int16_t x1 = frameX(m.toC);
      const int16_t y1 = frameY(m.toR);
      const int16_t x = x0 + (int16_t)((x1 - x0) * eased);
      const int16_t y = y0 + (int16_t)((y1 - y0) * eased);
      drawTileAt(x, y, m.value);
      drawn++;
    }
    tCompose += micros() - t0;
    t0 = micros();
    present();
    tBlit += micros() - t0;
    if (f == 1 && gestureT0)
    {
      moveFirstMs = (micros() - gestureT0) / 1000;
    }

    // Track the next gesture while this one animates. If the player is already
    // swiping again, settle the board right away and hand control back instead
    // of making that move wait for the remaining frames.
    t0 = micros();
    sampleTouch();
    tTouch += micros() - t0;

    if (releasePending || gestureActive())
    {
      // The next move paints the whole board with its own first frame, so there
      // is no need to spend another blit showing this one settled.
      if (timingOn)
      {
        Serial.printf("anim f=%u tiles=%u compose=%lu blit=%lu touch=%lu (cut)\n",
                      f, (unsigned)drawn, (unsigned long)(tCompose / 1000),
                      (unsigned long)(tBlit / 1000), (unsigned long)(tTouch / 1000));
      }
      return;
    }
  }

  if (timingOn)
  {
    Serial.printf("anim tiles=%u compose=%lu blit=%lu touch=%lu\n",
                  (unsigned)drawn, (unsigned long)(tCompose / 1000),
                  (unsigned long)(tBlit / 1000), (unsigned long)(tTouch / 1000));
  }
}

// Blocked swipe: bump the board a few pixels toward the swipe and spring back.
// The whole board image is composed shifted inside the frame buffer, so the
// bump is one normal blit and cannot leave stale pixels outside the board.
void nudge(uint8_t dir)
{
  const int16_t amount = 6;
  int16_t dx = 0;
  int16_t dy = 0;
  switch (dir)
  {
  case 0: dx = -amount; break; // left
  case 1: dx =  amount; break; // right
  case 2: dy = -amount; break; // up
  case 3: dy =  amount; break; // down
  }

  // Present the shifted image and return immediately; loop() holds it for a
  // moment and springs back, or abandons it as soon as the next swipe starts.
  drawBoard(dx, dy);
  bumpHeld = true;
  bumpAt = millis();

  if (timingOn) Serial.printf("blocked %u (bump %d,%d)\n", dir, dx, dy);
}

// Keeps the blocked-swipe bump visible for a short moment without blocking the
// input loop. A new gesture cancels it; its first frame repaints the board.
void pollBump()
{
  if (!bumpHeld)
  {
    return;
  }
  if (releasePending || gestureActive())
  {
    bumpHeld = false;
    return;
  }
  if (millis() - bumpAt >= 90)
  {
    bumpHeld = false;
    drawBoard();
  }
}

bool makeMove(uint8_t dir)
{
  MoveTimer mt;
  uint32_t out[ROWS][COLS];
  MoveResult res;
  move(board, dir, out, res);

  if (!res.changed)
  {
    nudge(dir);
    prevMoveEndMs = millis();
    return false;
  }

  memcpy(board, out, sizeof(board));
  score += res.gained;
  spawnTile();

  animateMove(res); // last frame paints the settled board

  // The score panel lives outside the framebuffer and costs several ms of
  // per-pixel panel writes. Queue it instead of drawing it here so a swipe that
  // is already waiting gets served first; loop() flushes it when idle.
  scoreDirty = true;

  if (timingOn)
  {
    Serial.printf("move %u  score=%lu  best=%lu\n", dir,
                  (unsigned long)score, (unsigned long)best);
  }

  const bool win = !winShown && maxTile(board) >= 2048;
  const bool stuck = !win && !hasMoves(board);
  if (!win && !stuck)
  {
    prevMoveEndMs = millis();
    return true;
  }

  // Before a panel or overlay is drawn, the score must be current.
  scoreDirty = false;
  drawScore();
  updateBest();
  prefs.putUInt("best", best);

  if (win)
  {
    winShown = true;
    state = WIN_SCREEN;
    drawOverlay("YOU WIN!", "tap to keep going", COL_TITLE);
    if (timingOn) Serial.println("win");
  }
  else
  {
    state = GAME_OVER;
    drawOverlay("GAME OVER", "tap to play again", RGB565(0xf6, 0x5e, 0x3b));
    if (timingOn) Serial.println("game over");
  }
  prevMoveEndMs = millis();
  return true;
}

void newGame()
{
  memset(board, 0, sizeof(board));
  score = 0;
  winShown = false;
  state = PLAYING;

  spawnTile();
  spawnTile();

  drawBoard();
  drawScore();
  drawBest();
  drawHint("swipe to move");

  Serial.println("new game");
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// Updates the gesture state without acting on it. Called from loop() and from
// each animation frame, so a swipe that starts while the previous move is still
// sliding is still captured from its true starting point.
void sampleTouch()
{
  uint16_t x, y;
  if (touchRead(x, y))
  {
    if (!touching)
    {
      touching = true;
      startX = lastX = x;
      startY = lastY = y;
      pendDownMs = millis();
      pendPeakX = 0;
      pendPeakY = 0;
      pendSamples = 1;
    }
    else
    {
      lastX = x;
      lastY = y;
      pendSamples++;
      const uint16_t ax = (uint16_t)((x > startX) ? (x - startX) : (startX - x));
      const uint16_t ay = (uint16_t)((y > startY) ? (y - startY) : (startY - y));
      if (ax > pendPeakX) pendPeakX = ax;
      if (ay > pendPeakY) pendPeakY = ay;
    }
  }
  else if (touching)
  {
    touching = false;
    pendingDx = (int16_t)lastX - (int16_t)startX;
    pendingDy = (int16_t)lastY - (int16_t)startY;
    releasePending = true;
  }
}

void processGesture(int16_t dx, int16_t dy)
{
  const uint32_t now = millis();
  GestureRecord rec;
  rec.gap = (uint16_t)(now - prevReleaseMs);
  rec.afterMove = (int16_t)((int32_t)now - (int32_t)prevMoveEndMs);
  rec.down = (uint16_t)(now - pendDownMs);
  rec.dx = dx;
  rec.dy = dy;
  rec.peakX = pendPeakX;
  rec.peakY = pendPeakY;
  rec.samples = pendSamples;
  rec.dir = 0xFF;
  rec.outcome = 0;
  rec.moveMs = 0;
  rec.firstMs = 0;
  prevReleaseMs = now;

  if (state == GAME_OVER)
  {
    rec.outcome = 3;
    logGesture(rec);
    newGame();
    return;
  }
  if (state == WIN_SCREEN)
  {
    rec.outcome = 3;
    logGesture(rec);
    state = PLAYING;
    drawBoard();
    drawScore();
    drawHint("swipe to move");
    return;
  }

  const int16_t adx = (dx < 0) ? -dx : dx;
  const int16_t ady = (dy < 0) ? -dy : dy;

  if ((adx > ady ? adx : ady) < SWIPE_MIN_PX)
  {
    logGesture(rec); // tap, not a swipe
    return;
  }

  uint8_t dir;
  if (adx >= ady)
  {
    dir = (dx < 0) ? 0 : 1; // left / right
  }
  else
  {
    dir = (dy < 0) ? 2 : 3; // up / down
  }
  rec.dir = dir;

  gestureT0 = micros();
  moveFirstMs = 0;
  const bool moved = makeMove(dir);
  rec.outcome = moved ? 1 : 2;
  rec.moveMs = (uint16_t)((micros() - gestureT0) / 1000);
  rec.firstMs = (uint16_t)moveFirstMs;
  logGesture(rec);
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(20); // never let a stalled USB host block the game
#endif
  delay(300);

  if (!gfx->begin())
  {
    Serial.println("Display init failed");
  }
  gfx->setBrightness(255);
  gfx->fillScreen(COL_BG);

  if (!createFrameBuffer())
  {
    Serial.println("Frame buffer allocation failed");
    while (true)
    {
      delay(1000);
    }
  }

  prefs.begin("game2048", false);
  best = prefs.getUInt("best", 0);

  randomSeed(micros());

  drawTitle();
  newGame();

  touchInit();
  Serial.println("2048 ready: swipe to move");

}

void loop()
{
  // Debug/test console: 1..4 = move left/right/up/down, b = bump, h = heap.
  while (Serial.available())
  {
    const int ch = Serial.read();
    if (ch >= '1' && ch <= '4') makeMove(ch - '1');
    else if (ch == 'b') nudge(0);
    else if (ch == 'h') Serial.printf("heap=%u\n", ESP.getFreeHeap());
    else if (ch == 'g')
    {
      Serial.printf("gestures=%u\n", glogCount);
      for (uint8_t i = 0; i < glogCount; i++)
      {
        const GestureRecord &r = glog[(glogNext + GESTURE_LOG - glogCount + i) % GESTURE_LOG];
        static const char *DIRS = "LRUD";
        static const char *OUTS[] = {"TAP", "MOVE", "BLOCK", "OVERLAY"};
        Serial.printf("%u gap=%u afterMove=%d down=%u d=(%d,%d) peak=(%u,%u) n=%u %c %s move=%u first=%u\n",
                      i, r.gap, r.afterMove, r.down, r.dx, r.dy, r.peakX, r.peakY,
                      r.samples, (r.dir < 4) ? DIRS[r.dir] : '-', OUTS[r.outcome & 3],
                      r.moveMs, r.firstMs);
      }
    }
    else if (ch == 'c')
    {
      glogCount = 0;
      glogNext = 0;
      Serial.println("gesture log cleared");
    }
    else if (ch == 'm')
    {
      Serial.printf("movetime last=%lu max=%lu ring:", (unsigned long)(moveUsLast / 1000),
                    (unsigned long)(moveUsMax / 1000));
      for (uint8_t i = 0; i < 8; i++)
      {
        Serial.printf(" %lu", (unsigned long)(moveUsRing[(moveUsPos + i) & 7] / 1000));
      }
      Serial.println();
    }
    else if (ch == 't') { timingOn = !timingOn; Serial.printf("timing=%u\n", timingOn); }
    else if (ch == 's')
    {
      uint32_t a = micros();
      gfx->fillRoundRect(SCORE_X, SCORE_Y, SCORE_W, SCORE_H, 8, COL_PANEL);
      uint32_t b = micros();
      gfx->setTextColor(COL_MUTED);
      gfx->setTextSize(2);
      gfx->setCursor(SCORE_X + (SCORE_W - 60) / 2, 14);
      gfx->print("SCORE");
      uint32_t c = micros();
      char v[16];
      snprintf(v, sizeof(v), "%lu", (unsigned long)score);
      const uint8_t sz = (strlen(v) <= 6) ? 3 : 2;
      const int16_t vw = strlen(v) * 6 * sz;
      gfx->setTextColor(COL_LIGHT_TEXT);
      gfx->setTextSize(sz);
      gfx->setCursor(SCORE_X + (SCORE_W - vw) / 2, (sz == 3) ? 34 : 38);
      gfx->print(v);
      uint32_t d = micros();
      Serial.printf("scorebg=%lu label=%lu value=%lu (\"%s\")\n",
                    (unsigned long)((b - a) / 1000), (unsigned long)((c - b) / 1000),
                    (unsigned long)((d - c) / 1000), v);
    }
    else if (ch == 'e')
    {
      uint32_t a = micros();
      drawBest();
      uint32_t b = micros();
      Serial.printf("best=%lu\n", (unsigned long)((b - a) / 1000));
    }
    else if (ch == 'n') newGame();
  }

  sampleTouch();
  if (releasePending)
  {
    releasePending = false;
    bumpHeld = false; // the new move repaints the board; drop any held bump
    processGesture(pendingDx, pendingDy);
  }
  pollBump();

  if (scoreDirty)
  {
    scoreDirty = false;
    drawScore();
    updateBest();
  }

  delay(2);
}
