// Wahoo BLE heart rate monitor — Waveshare ESP32-S3-Touch-AMOLED-1.64.
// CO5300 280x456 QSPI, FT3168 touch on SDA 47 / SCL 48.
// BLE work runs separately so scanning/connecting never blocks the display.

#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <Wire.h>
#include "heart_rate.h"

// Optional: pin the connection to one strap's advertised BLE address. This
// bypasses the name filter (also works with other standard BLE HR straps).
// Leave empty to find Wahoo / TICKR / TRACKR heart rate advertisements.
static const char *TARGET_ADDRESS = "";
static const uint32_t STALE_MS = 5000;
static const uint32_t RECONNECT_MS = 20000;
static const int WIDTH = 280;
static const int HEIGHT = 456;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(46, 10, 11, 12, 13, 14);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, 21, 0, WIDTH, HEIGHT, 20, 0);
Arduino_Canvas *frame = new Arduino_Canvas(WIDTH, HEIGHT, nullptr);
static BLEUUID hrService(uint16_t(0x180D));
static BLEUUID hrMeasurement(uint16_t(0x2A37));

enum class Phase { SCANNING, CONNECTING, CONNECTED, RETRYING };

struct MonitorState {
  Phase phase = Phase::SCANNING;
  char message[32] = "Looking for Wahoo";
  char name[32] = "No strap connected";
  char address[18] = "";
  heart_rate::Measurement reading;
  bool hasReading = false;
  uint32_t receivedAt = 0;
  uint32_t packets = 0;
};

static MonitorState state;
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static bool rescanRequested = false;

MonitorState snapshot() {
  portENTER_CRITICAL(&stateMux);
  MonitorState copy = state;
  portEXIT_CRITICAL(&stateMux);
  return copy;
}

void setPhase(Phase phase, const char *message) {
  portENTER_CRITICAL(&stateMux);
  state.phase = phase;
  snprintf(state.message, sizeof(state.message), "%s", message);
  if (phase != Phase::CONNECTED) state.hasReading = false;
  portEXIT_CRITICAL(&stateMux);
  Serial.println(message);
}

bool takeRescanRequest() {
  portENTER_CRITICAL(&stateMux);
  const bool requested = rescanRequested;
  rescanRequested = false;
  portEXIT_CRITICAL(&stateMux);
  return requested;
}

void onMeasurement(BLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
  heart_rate::Measurement value;
  if (!heart_rate::parseMeasurement(data, length, value)) return;
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateMux);
  if (state.phase == Phase::CONNECTED) {
    state.reading = value;
    state.receivedAt = now;
    state.hasReading = true;
    ++state.packets;
  }
  portEXIT_CRITICAL(&stateMux);
}

void disconnectStrap(BLEClient *client) {
  if (client->isConnected()) client->disconnect();
  // Do not reuse the client while a disconnect is still being processed.
  while (client->isConnected()) delay(50);
}

void retryPause() {
  for (int i = 0; i < 20; ++i) {
    if (takeRescanRequest()) break;
    delay(100);
  }
}

void bluetoothTask(void *) {
  BLEDevice::init("Waveshare Heart Rate");
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true); // Request scan-response data, including names.
  scan->setInterval(100);
  scan->setWindow(80);
  // Reuse one client rather than leaking a new GATT client on each reconnect.
  BLEClient *client = BLEDevice::createClient();

  while (true) {
    takeRescanRequest();
    setPhase(Phase::SCANNING, "Looking for Wahoo");
    BLEScanResults *results = scan->start(4, false);
    int selected = -1;
    if (results) {
      for (int i = 0; i < results->getCount(); ++i) {
        BLEAdvertisedDevice device = results->getDevice(i);
        const String name = device.getName();
        const String address = device.getAddress().toString();
        const bool hasHR = device.isAdvertisingService(hrService);
        if (hasHR || heart_rate::isWahooName(name.c_str())) {
          Serial.printf("HR candidate: %s [%s], HR service=%d\n",
                        name.c_str(), address.c_str(), hasHR);
        }
        const bool matches = TARGET_ADDRESS[0]
            ? address.equalsIgnoreCase(TARGET_ADDRESS)
            : hasHR && heart_rate::isWahooName(name.c_str());
        if (matches && selected < 0) selected = i;
      }
    }
    if (selected < 0) {
      scan->clearResults();
      setPhase(Phase::RETRYING, "Wear / wake the strap");
      retryPause();
      continue;
    }

    // Own this copy before freeing the scan's results, including address type.
    BLEAdvertisedDevice device = results->getDevice(selected);
    scan->clearResults();
    const String name = device.getName();
    const String address = device.getAddress().toString();
    portENTER_CRITICAL(&stateMux);
    snprintf(state.name, sizeof(state.name), "%.21s", name.length() ? name.c_str() : "Heart rate strap");
    snprintf(state.address, sizeof(state.address), "%s", address.c_str());
    portEXIT_CRITICAL(&stateMux);
    setPhase(Phase::CONNECTING, "Connecting to strap");

    if (!client->connectTimeout(&device, 8000)) {
      disconnectStrap(client);
      setPhase(Phase::RETRYING, "Connection failed");
      retryPause();
      continue;
    }

    // Force rediscovery on every connection; do not reuse old GATT handles.
    client->getServices();
    BLERemoteService *service = client->getService(hrService);
    BLERemoteCharacteristic *measurement = service ? service->getCharacteristic(hrMeasurement) : nullptr;
    if (!measurement || !measurement->canNotify() ||
        !measurement->getDescriptor(BLEUUID(uint16_t(0x2902)))) {
      setPhase(Phase::RETRYING, "No HR notifications");
      disconnectStrap(client);
      retryPause();
      continue;
    }

    setPhase(Phase::CONNECTED, "Waiting for readings");
    const uint32_t connectedAt = millis();
    // Registers the callback AND writes 0x0001 to the notification descriptor.
    measurement->registerForNotify(onMeasurement);
    const char *reason = "Strap disconnected";
    while (client->isConnected()) {
      if (takeRescanRequest()) {
        reason = "Reconnecting";
        break;
      }
      const MonitorState current = snapshot();
      const uint32_t lastPacket = current.hasReading ? current.receivedAt : connectedAt;
      if (heart_rate::elapsed(millis(), lastPacket, RECONNECT_MS)) {
        reason = "No data - reconnecting";
        break;
      }
      delay(100);
    }
    setPhase(Phase::RETRYING, reason);
    disconnectStrap(client);
    retryPause();
  }
}

// FT3168: only touch-down is needed for the reconnect control.
bool touchDown() {
  Wire.beginTransmission(0x38);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(0x38, 1) != 1) return false;
  return (Wire.read() & 0x0F) != 0;
}

static const uint16_t C_BG = RGB565_BLACK;
static const uint16_t C_TEXT = RGB565(235, 240, 245);
static const uint16_t C_DIM = RGB565(130, 145, 160);
static const uint16_t C_GREEN = RGB565(40, 230, 150);
static const uint16_t C_AMBER = RGB565(255, 185, 65);
static const uint16_t C_RED = RGB565(255, 65, 100);

void centered(const char *text, int y, int size, uint16_t color) {
  frame->setTextSize(size);
  frame->setTextColor(color);
  frame->setCursor((WIDTH - int(strlen(text)) * 6 * size) / 2, y);
  frame->print(text);
}

void drawScreen(const MonitorState &current) {
  // Read the clock after the snapshot: a notification may have arrived between
  // the loop's timestamp and the snapshot, making unsigned age underflow.
  const uint32_t now = millis();
  const bool connected = current.phase == Phase::CONNECTED;
  const bool fresh = connected && current.hasReading &&
      !heart_rate::elapsed(now, current.receivedAt, STALE_MS);
  const bool live = fresh && heart_rate::usable(current.reading);
  const uint16_t statusColor = live ? C_GREEN : C_AMBER;
  frame->fillScreen(C_BG);
  centered("HEART RATE", 26, 3, C_TEXT);
  frame->fillRoundRect(32, 70, 216, 28, 14, RGB565(18, 27, 35));
  const char *label = current.phase == Phase::SCANNING ? "SEARCHING" :
      current.phase == Phase::CONNECTING ? "CONNECTING" :
      !connected ? "RETRYING" : !current.hasReading ? "WAITING FOR DATA" :
      !fresh ? "SIGNAL STALE" : live ? "LIVE" : "CHECK STRAP";
  centered(label, 77, 2, statusColor);

  // Flash on received measurements, not a simulated ECG or beat detector.
  const uint16_t heartColor = live && uint32_t(now - current.receivedAt) < 200
      ? C_RED : RGB565(95, 25, 45);
  frame->fillCircle(129, 131, 12, heartColor);
  frame->fillCircle(151, 131, 12, heartColor);
  frame->fillTriangle(117, 135, 163, 135, 140, 160, heartColor);
  char text[32];
  if (live) snprintf(text, sizeof(text), "%u", current.reading.bpm);
  else snprintf(text, sizeof(text), "--");
  centered(text, 183, strlen(text) > 3 ? 9 : 12, live ? C_TEXT : C_DIM);
  centered("BPM", 287, 2, C_DIM);

  const char *detail = current.message;
  if (connected) {
    if (!current.hasReading) detail = "Waiting for readings";
    else if (!fresh) detail = "Waiting for signal";
    else if (current.reading.contactSupported && !current.reading.contactDetected)
      detail = "Check skin contact";
    else if (!current.reading.bpm) detail = "Waiting for pulse";
    else if (current.reading.hasRR) {
      snprintf(text, sizeof(text), "RR interval: %u ms", current.reading.rrMs);
      detail = text;
    } else detail = "Receiving heart rate";
  }
  centered(detail, 329, 1, statusColor);
  centered(current.name, 356, 2, C_TEXT);
  centered(current.address, 380, 1, C_DIM);
  if (connected && current.hasReading) {
    snprintf(text, sizeof(text), "Last update: %lu s ago",
             (unsigned long)(uint32_t(now - current.receivedAt) / 1000));
    centered(text, 398, 1, C_DIM);
  }
  centered("Tap anywhere to reconnect", 425, 1, C_DIM);
  gfx->draw16bitRGBBitmap(0, 0, frame->getFramebuffer(), WIDTH, HEIGHT);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);
  if (!gfx->begin()) {
    Serial.println("Display init failed");
    while (true) delay(1000);
  }
  gfx->setBrightness(220);
  gfx->fillScreen(C_BG);
  if (!psramFound() || !frame->begin()) {
    gfx->setTextColor(C_RED);
    gfx->setTextSize(2);
    gfx->setCursor(12, 180);
    gfx->print("Enable PSRAM");
    while (true) delay(1000);
  }
  frame->setTextWrap(false);
  Wire.begin(47, 48, 400000);
  Wire.beginTransmission(0x38);
  Wire.write(0x00);
  Wire.write(0x00); // Touch normal mode.
  Wire.endTransmission();
  drawScreen(snapshot());
  if (xTaskCreate(bluetoothTask, "heart-rate-ble", 8192, nullptr, 1, nullptr) != pdPASS) {
    setPhase(Phase::RETRYING, "BLE task failed - reboot");
  }
}

void loop() {
  static bool wasDown = false;
  static uint32_t lastTouch = 0;
  static uint32_t lastDraw = 0;
  const uint32_t now = millis();
  const bool down = touchDown();
  if (down && !wasDown && heart_rate::elapsed(now, lastTouch, 500)) {
    portENTER_CRITICAL(&stateMux);
    rescanRequested = true;
    portEXIT_CRITICAL(&stateMux);
    lastTouch = now;
  }
  wasDown = down;
  if (heart_rate::elapsed(now, lastDraw, 100)) {
    lastDraw = now;
    drawScreen(snapshot());
  }
  delay(10);
}
