// Floating, mouse-controlled windows on Waveshare ESP32-S3-Touch-AMOLED-1.64.
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include "ble_mouse.h"
#include "window_manager.h"

using namespace window_manager;

class PSRAMCanvas : public Arduino_Canvas {
public:
  PSRAMCanvas() : Arduino_Canvas(window_manager::WIDTH, window_manager::HEIGHT, nullptr) {}
  bool begin(int32_t = GFX_NOT_DEFINED) override {
    _framebuffer = static_cast<uint16_t *>(heap_caps_aligned_alloc(
        16, size_t(window_manager::WIDTH) * window_manager::HEIGHT * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return _framebuffer != nullptr;
  }
};

Arduino_DataBus *bus = new Arduino_ESP32QSPI(46, 10, 11, 12, 13, 14);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, 21, 0, WIDTH, HEIGHT, 20, 0);
static PSRAMCanvas scene, frame;
static Desktop desktop;
static uint8_t page = 0, previousPage = 0;
static wm_ble::State displayed, previousUI;
static bool screenValid = false;
static uint32_t sceneRevision = 0;
static uint32_t renderedFrames = 0, lastRenderUs = 0, maxRenderUs = 0;
alignas(16) static uint16_t uploadPixels[WIDTH * 16];

static const uint16_t C_BG = RGB565(8, 14, 23);
static const uint16_t C_GRID = RGB565(23, 35, 48);
static const uint16_t C_BODY = RGB565(24, 35, 49);
static const uint16_t C_SHADOW = RGB565(2, 5, 9);
static const uint16_t C_TEXT = RGB565(225, 237, 248);
static const uint16_t C_DIM = RGB565(115, 139, 160);
static const uint16_t C_ACCENT = RGB565(65, 220, 195);
static const uint16_t C_AMBER = RGB565(255, 185, 65);
static const uint16_t COLORS[] = {
  RGB565(64, 150, 200), RGB565(160, 112, 210), RGB565(210, 138, 73),
  RGB565(60, 167, 135), RGB565(204, 105, 132), RGB565(93, 132, 220)
};

void text(Arduino_Canvas &canvas, const char *label, int x, int y, int size, uint16_t color) {
  canvas.setTextSize(size);
  canvas.setTextColor(color);
  canvas.setCursor(x, y);
  canvas.print(label);
}

void centered(const char *label, int y, int size, uint16_t color) {
  text(scene, label, (WIDTH - int(strlen(label)) * 6 * size) / 2, y, size, color);
}

void button(const char *label, Rect rect, uint16_t color = C_ACCENT) {
  scene.fillRoundRect(rect.x, rect.y, rect.w, rect.h, 7, C_BODY);
  text(scene, label, rect.x + (rect.w - int(strlen(label)) * 6) / 2,
       rect.y + (rect.h - 8) / 2, 1, color);
}

void fillBackground() {
  uint32_t *pixels = reinterpret_cast<uint32_t *>(scene.getFramebuffer());
  const uint32_t pair = (uint32_t(C_BG) << 16) | C_BG;
  for (unsigned i = 0; i < WIDTH * HEIGHT / 2; ++i) pixels[i] = pair;
}

uint32_t randomWord(uint32_t &seed) {
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return seed;
}

void drawWindow(const Window &window, bool active) {
  const Rect rect = window.bounds();
  const uint16_t accent = COLORS[(window.id - 1) % 6];
  scene.fillRect(rect.x + SHADOW, rect.y + SHADOW, rect.w, rect.h, C_SHADOW);
  scene.fillRect(rect.x, rect.y, rect.w, rect.h, C_BODY);
  scene.fillRect(rect.x, rect.y, rect.w, TITLE_HEIGHT, active ? accent : RGB565(36, 52, 69));
  scene.drawRect(rect.x, rect.y, rect.w, rect.h, active ? C_ACCENT : RGB565(58, 77, 96));
  scene.drawFastHLine(rect.x, rect.y + TITLE_HEIGHT, rect.w, RGB565(58, 77, 96));
  char title[24];
  snprintf(title, sizeof(title), "WINDOW %02u", window.id);
  text(scene, title, rect.x + 10, rect.y + 9, 1, C_TEXT);
  const Rect close = window.closeButton();
  scene.fillRoundRect(close.x, close.y, close.w, close.h, 4, RGB565(131, 55, 67));
  const int cx = close.x + close.w / 2, cy = close.y + close.h / 2;
  scene.drawLine(cx - 4, cy - 4, cx + 4, cy + 4, C_TEXT);
  scene.drawLine(cx + 4, cy - 4, cx - 4, cy + 4, C_TEXT);
  static const char *const lines[] = {
    "A small idea lives here.", "Clouds drift past the moon.", "Nothing urgent. Just bits.",
    "Hello from another window.", "Pixels make a tiny world.", "Somewhere, a clock ticks.",
    "Coffee, code, and starlight.", "This note is going places.", "A pocket-sized desktop.",
    "The story continues here.", "Keep a little room for fun.", "One thought leads to more."
  };
  uint32_t seed = window.textSeed;
  for (int line = 0; line < 7; ++line) {
    char clipped[27]; // Never let text paint outside its window's damage bounds.
    snprintf(clipped, sizeof(clipped), "%.26s", lines[randomWord(seed) % 12]);
    text(scene, clipped, rect.x + 10, rect.y + 40 + line * 13, 1, C_TEXT);
  }
  text(scene, active ? "ACTIVE / DRAG TITLE BAR" : "CLICK TO BRING FORWARD",
       rect.x + 10, rect.bottom() - 17, 1, active ? C_ACCENT : C_DIM);
}

void drawDesktop(const wm_ble::State &current) {
  fillBackground();
  for (int y = 83; y < DESKTOP_BOTTOM; y += 24)
    for (int x = 12; x < WIDTH; x += 24) scene.drawPixel(x, y, C_GRID);
  centered("MINI DESKTOP", 16, 2, C_TEXT);
  centered(current.phase == wm_ble::Phase::CONNECTED ? "Drag titles / click to focus" : current.message,
           38, 1, current.phase == wm_ble::Phase::CONNECTED ? C_DIM : C_AMBER);
  scene.fillCircle(16, 57, 3, current.phase == wm_ble::Phase::CONNECTED ? C_ACCENT : C_AMBER);
  text(scene, current.name, 27, 53, 1, C_DIM);
  scene.drawFastHLine(0, 69, WIDTH, C_GRID);
  for (unsigned i = 0; i < desktop.count; ++i) drawWindow(desktop.windows[i], i + 1 == desktop.count);
  scene.fillRect(0, DESKTOP_BOTTOM, WIDTH, HEIGHT - DESKTOP_BOTTOM, RGB565(14, 23, 34));
  scene.drawFastHLine(0, DESKTOP_BOTTOM, WIDTH, C_GRID);
  char count[24];
  snprintf(count, sizeof(count), "%u WINDOW%s", desktop.count, desktop.count == 1 ? "" : "S");
  text(scene, count, 12, 430, 1, C_DIM);
  const Rect add = addButton();
  const bool hover = add.contains(desktop.cursor.x, desktop.cursor.y);
  const uint16_t addColor = desktop.count == MAX_WINDOWS ? C_DIM : C_ACCENT;
  scene.fillRoundRect(add.x, add.y, add.w, add.h, 8, hover ? RGB565(38, 67, 74) : C_BODY);
  scene.drawFastHLine(add.x + 11, add.y + 14, 14, addColor);
  scene.drawFastVLine(add.x + 17, add.y + 8, 14, addColor);
  button("Pair", pairButton(), pairButton().contains(desktop.cursor.x, desktop.cursor.y) ? C_TEXT : C_DIM);
  if (desktop.count == MAX_WINDOWS) text(scene, "LIMIT", 169, 430, 1, C_AMBER);
}

void drawPairing(const wm_ble::State &current) {
  fillBackground();
  centered("MINI DESKTOP", 22, 2, C_TEXT);
  centered(current.phase == wm_ble::Phase::CONNECTING ? "CONNECTING" : "PAIR A MOUSE", 58, 2, C_AMBER);
  centered(current.message, 88, 1, C_DIM);
  centered("Hold the mouse's pairing button", 111, 1, C_DIM);
  centered("Then tap its name below", 126, 1, C_DIM);
  if (page * 4 >= current.count) page = 0;
  if (!current.count) {
    centered(current.scanning ? "Scanning for BLE mice..." : "No HID devices found yet", 220, 1, C_TEXT);
    centered("Bluetooth LE only", 244, 1, C_DIM);
  }
  for (uint8_t row = 0; row < 4 && page * 4 + row < current.count; ++row) {
    const auto &peer = current.peers[page * 4 + row];
    const int y = 153 + row * 51;
    scene.fillRoundRect(12, y, 256, 45, 8, C_BODY);
    text(scene, peer.name, 22, y + 8, 1, C_TEXT);
    text(scene, peer.address, 22, y + 26, 1, C_DIM);
    char signal[16];
    snprintf(signal, sizeof(signal), "%d dB", peer.rssi);
    text(scene, signal, 220, y + 26, 1, C_DIM);
  }
  centered(current.scanning ? "Scanning..." : "Tap a device to connect", 363, 1, C_DIM);
  button("Rescan", Rect(12, 380, 78, 36));
  button(current.count > 4 ? "Next page" : "BLE only", Rect(101, 380, 78, 36));
  button(current.hasSaved ? "Cancel" : "Pairing", Rect(190, 380, 78, 36));
  if (current.hasSaved) button("Forget saved mouse", Rect(42, 420, 196, 36), C_AMBER);
}

void drawCursor(uint16_t color) {
  const int x = desktop.cursor.x, y = desktop.cursor.y;
  frame.fillTriangle(x, y, x, y + 17, x + 11, y + 11, C_SHADOW);
  frame.fillTriangle(x + 1, y + 2, x + 1, y + 14, x + 9, y + 10, color);
  frame.drawLine(x + 5, y + 10, x + 9, y + 18, color);
  frame.drawLine(x + 6, y + 10, x + 10, y + 18, color);
}

void copySceneRect(Rect rect) {
  for (int row = rect.y; row < rect.bottom(); ++row)
    memcpy(frame.getFramebuffer() + row * WIDTH + rect.x,
           scene.getFramebuffer() + row * WIDTH + rect.x, size_t(rect.w) * 2);
}

void pushRect(Rect rect) {
  uint16_t *pixels = frame.getFramebuffer();
  if (rect.w == WIDTH) {
    gfx->draw16bitRGBBitmap(0, rect.y, pixels + rect.y * WIDTH, WIDTH, rect.h);
    return;
  }
  const int maxRows = stripRows(rect.w, int(sizeof(uploadPixels) / sizeof(uploadPixels[0])));
  for (int row = 0; row < rect.h; row += maxRows) {
    const int rows = rect.h - row < maxRows ? rect.h - row : maxRows;
    const Rect strip(rect.x, rect.y + row, rect.w, rows);
    packRect(pixels, WIDTH, strip, uploadPixels);
    gfx->draw16bitRGBBitmap(strip.x, strip.y, uploadPixels, strip.w, strip.h);
  }
}

bool interfaceChanged(const wm_ble::State &current) {
  if (!screenValid || current.phase != previousUI.phase || current.pairing != previousUI.pairing ||
      strcmp(current.name, previousUI.name) || strcmp(current.message, previousUI.message)) return true;
  return current.pairing && (page != previousPage || current.scanning != previousUI.scanning ||
      current.hasSaved != previousUI.hasSaved || current.count != previousUI.count ||
      memcmp(current.peers, previousUI.peers, sizeof(current.peers)));
}

void render(const wm_ble::State &current) {
  const uint32_t started = micros();
  Damage damage = desktop.takeDamage();
  const bool full = interfaceChanged(current);
  if (full || sceneRevision != desktop.revision) {
    if (current.pairing) drawPairing(current);
    else drawDesktop(current);
    sceneRevision = desktop.revision;
  }
  if (full) { damage = Damage(); damage.add(Rect(0, 0, WIDTH, HEIGHT)); }
  for (unsigned i = 0; i < damage.count; ++i) copySceneRect(damage.regions[i]);
  if (!current.pairing && damage.count)
    drawCursor(current.phase == wm_ble::Phase::CONNECTED ? C_ACCENT : C_DIM);
  for (unsigned i = 0; i < damage.count; ++i) pushRect(damage.regions[i]);
  previousUI = current;
  previousPage = page;
  screenValid = true;
  if (damage.count) {
    ++renderedFrames;
    lastRenderUs = uint32_t(micros() - started);
    if (lastRenderUs > maxRenderUs) maxRenderUs = lastRenderUs;
  }
}

bool readTouch(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(0x38);
  Wire.write(0x02);
  if (Wire.endTransmission(false) || Wire.requestFrom(0x38, 5) != 5) return false;
  uint8_t bytes[5];
  for (uint8_t &byte : bytes) byte = Wire.read();
  if (!(bytes[0] & 15) || (bytes[1] >> 6) == 1) return false;
  x = ((bytes[1] & 15) << 8) | bytes[2];
  y = ((bytes[3] & 15) << 8) | bytes[4];
  return x < WIDTH && y < HEIGHT;
}

void handleTap(uint16_t x, uint16_t y) {
  using wm_ble::Action;
  if (!displayed.pairing) {
    if (pairButton().contains(x, y)) { page = 0; desktop.cancelInput(); wm_ble::request(Action::PAIR); }
    return;
  }
  if (y >= 420 && displayed.hasSaved) { wm_ble::request(Action::FORGET); return; }
  if (y >= 380 && y < 416) {
    if (x < 96) wm_ble::request(Action::RESCAN);
    else if (x < 185 && displayed.count > 4) page = (page + 1) % 2;
    else if (x >= 185 && displayed.hasSaved) wm_ble::request(Action::CANCEL);
    return;
  }
  if (displayed.phase == wm_ble::Phase::CONNECTING || x < 12 || x >= 268 || y < 153 || y >= 357) return;
  const uint8_t row = (y - 153) / 51, index = page * 4 + row;
  if ((y - 153) % 51 < 45 && index < displayed.count) wm_ble::request(Action::SELECT, displayed.peers[index]);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  if (!gfx->begin()) { Serial.println("Display init failed"); while (true) delay(1000); }
  gfx->setBrightness(200);
  gfx->fillScreen(RGB565_BLACK);
  if (!psramFound() || !scene.begin() || !frame.begin()) {
    gfx->setTextSize(2);
    gfx->setTextColor(C_AMBER);
    gfx->setCursor(12, 180);
    gfx->print("Enable PSRAM");
    while (true) delay(1000);
  }
  scene.setTextWrap(false);
  frame.setTextWrap(false);
  desktop = Desktop(esp_random());
  Wire.begin(47, 48, 400000);
  Wire.setTimeOut(20);
  Wire.beginTransmission(0x38);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();
  wm_ble::begin();
}

void loop() {
  static uint32_t lastDrawUs = 0, lastPoll = 0, lastTouch = 0;
  static bool wasDown = false;
  const uint32_t now = millis();
  wm_ble::tick();
  const wm_ble::State current = wm_ble::snapshot();
  wm_ble::Input input;
  for (unsigned i = 0; i < 256 && wm_ble::nextInput(input); ++i) {
    if (input.reset) desktop.cancelInput();
    else if (!current.pairing && current.phase == wm_ble::Phase::CONNECTED &&
             desktop.input(input.motion.dx, input.motion.dy, input.motion.buttons)) {
      page = 0;
      wm_ble::request(wm_ble::Action::PAIR);
    }
  }
  if (uint32_t(now - lastPoll) >= 8) {
    lastPoll = now;
    uint16_t x = 0, y = 0;
    const bool down = readTouch(x, y);
    if (down && !wasDown && uint32_t(now - lastTouch) >= 300) { handleTap(x, y); lastTouch = now; }
    wasDown = down;
  }
  const uint32_t nowUs = micros();
  if (uint32_t(nowUs - lastDrawUs) >= 16667) {
    lastDrawUs = nowUs;
    displayed = wm_ble::snapshot();
    render(displayed);
  }
  if (Serial.available() && Serial.read() == '?') {
    wm_ble::printStatus();
    Serial.printf("Desktop: windows=%u, active=%u, dragging=%u, cursor=(%d,%d), frames=%lu, last/max=%.2f/%.2f ms\n",
                  desktop.count, desktop.activeWindowID(), desktop.isDragging(), desktop.cursor.x, desktop.cursor.y,
                  (unsigned long)renderedFrames, lastRenderUs / 1000.0, maxRenderUs / 1000.0);
  }
  delay(1);
}
