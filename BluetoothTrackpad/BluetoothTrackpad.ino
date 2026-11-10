// BLE HID peripheral — Waveshare ESP32-S3-Touch-AMOLED-1.64.
#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <Wire.h>
#include <atomic>
#include "gestures.h"

#if !defined(CONFIG_NIMBLE_ENABLED)
#error Use the ESP32-S3 core 3.3.12 NimBLE stack.
#endif

Arduino_DataBus *bus = new Arduino_ESP32QSPI(46, 10, 11, 12, 13, 14);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, 21, 0, 280, 456, 20, 0);
BLEHIDDevice *hid = nullptr;
BLECharacteristic *input = nullptr;
std::atomic<bool> connected{false}, encrypted{false}, subscribed{false};
std::atomic<uint32_t> session{0};
trackpad::Gestures gestures;

// Three buttons, signed relative X/Y, and vertical wheel. Report ID is in the
// Report Reference descriptor; notification payloads contain only these 4 bytes.
static uint8_t reportMap[] = {
  0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01,
  0x09, 0x01, 0xa1, 0x00,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
  0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
  0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
  0xc0, 0xc0
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server, ble_gap_conn_desc *desc) override {
    encrypted = false;
    subscribed = false;
    connected = true;
    ++session;
    server->requestConnParams(desc->conn_handle, 6, 12, 0, 400);
    Serial.println("Computer connected; waiting for encryption and HID subscription");
  }
  void onDisconnect(BLEServer *, ble_gap_conn_desc *) override {
    connected = false;
    encrypted = false;
    subscribed = false;
    ++session;
    Serial.println("Disconnected; advertising again");
  }
};

class SecurityCallbacks : public BLESecurityCallbacks {
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return false; }
  void onAuthenticationComplete(ble_gap_conn_desc *desc, int status) override {
    encrypted = status == 0 && desc && desc->sec_state.encrypted;
    Serial.printf("Authentication: %s (0x%04x)\n", encrypted ? "OK" : "failed", status);
  }
};

class InputCallbacks : public BLECharacteristicCallbacks {
  void onSubscribe(BLECharacteristic *, ble_gap_conn_desc *, uint16_t value) override {
    subscribed = (value & 1) != 0;
  }
};

static ServerCallbacks serverCallbacks;
static SecurityCallbacks securityCallbacks;
static InputCallbacks inputCallbacks;

bool ready() { return connected && encrypted && subscribed; }

void sendReport(uint8_t buttons, int dx = 0, int dy = 0, int wheel = 0) {
  if (!ready()) return;
  uint8_t report[] = {buttons, uint8_t(int8_t(dx)), uint8_t(int8_t(dy)), uint8_t(int8_t(wheel))};
  input->setValue(report, sizeof(report));
  input->notify();
}

// Failed I2C reads are not finger-up events (which could cause false clicks).
bool readTouch(bool &down, int &x, int &y) {
  Wire.beginTransmission(0x38);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(0x38, 5) != 5) return false;
  uint8_t b[5];
  for (auto &v : b) v = Wire.read();
  down = (b[0] & 0x0f) != 0;
  x = ((b[1] & 0x0f) << 8) | b[2];
  y = ((b[3] & 0x0f) << 8) | b[4];
  if (down && (x >= 280 || y >= 456)) return false;
  return true;
}

const uint16_t BG = RGB565(10, 14, 22);
const uint16_t DIM = RGB565(140, 155, 175);
const uint16_t CYAN = RGB565(70, 215, 255);

void text(int x, int y, const char *label, uint16_t color, int size = 1) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->print(label);
}

void drawButtons() {
  gfx->fillRoundRect(12, 370, 78, 50, 8, RGB565(32, 44, 60));
  gfx->fillRoundRect(98, 370, 78, 50, 8, RGB565(32, 44, 60));
  gfx->fillRoundRect(184, 370, 84, 50, 8,
                     gestures.dragging ? RGB565(0, 110, 100) : RGB565(32, 44, 60));
  text(27, 390, "LEFT", RGB565_WHITE, 2);
  text(107, 390, "RIGHT", RGB565_WHITE, 2);
  text(202, 383, "DRAG", RGB565_WHITE, 2);
  text(214, 405, gestures.dragging ? "ON" : "OFF", DIM);
}

void drawScreen() {
  gfx->fillScreen(BG);
  text(14, 20, "TRACKPAD", CYAN, 3);
  text(14, 84, "Pair: ESP32 Trackpad", DIM);
  gfx->drawRoundRect(12, 112, 218, 242, 12, RGB565(50, 65, 85));
  text(52, 219, "MOVE YOUR FINGER", DIM);
  text(67, 239, "tap to click", DIM);
  gfx->fillRoundRect(238, 112, 30, 242, 10, RGB565(25, 40, 55));
  text(250, 128, "^", CYAN);
  text(250, 332, "v", CYAN);
  drawButtons();
  text(18, 435, "DRAG: tap on, move, tap off", DIM);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  Wire.begin(47, 48, 400000);
  Wire.setTimeOut(20);
  gfx->begin();
  gfx->setBrightness(200);
  drawScreen();

  BLEDevice::init("ESP32 Trackpad");
  BLEDevice::setSecurityCallbacks(&securityCallbacks);
  BLESecurity::setCapability(ESP_IO_CAP_NONE);
  BLESecurity::setAuthenticationMode(true, false, true);
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(&serverCallbacks);
  server->advertiseOnDisconnect(true);
  hid = new BLEHIDDevice(server);
  // Core 3.3.12 creates this optional characteristic only via the getter.
  // The manufacturer(String) setter dereferences an uninitialized pointer
  // unless manufacturer() has been called first.
  hid->manufacturer()->setValue("ESP32 Trackpad");
  hid->hidInfo(0, 0x02); // Normally connectable; no remote wake claim.
  input = hid->inputReport(1);
  input->setCallbacks(&inputCallbacks);
  uint8_t initial[] = {0, 0, 0, 0};
  input->setValue(initial, sizeof(initial));
  hid->reportMap(reportMap, sizeof(reportMap));
  hid->startServices();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->setAppearance(HID_MOUSE);
  advertising->addServiceUUID(hid->hidService()->getUUID());
  advertising->setScanResponse(true);
  advertising->setMinPreferred(6);
  advertising->setMaxPreferred(12);
  const bool advertisingStarted = advertising->start();
  Serial.printf("Advertising: %s\n", advertisingStarted ? "started" : "FAILED");
  if (advertisingStarted) {
    Serial.println("Pair ESP32 Trackpad from your computer's Bluetooth settings");
  }
}

void loop() {
  static uint32_t seenSession = 0, clickAt = 0, lastGoodTouch = 0;
  static bool wasReady = false, waitForLift = true;
  static uint8_t click = 0;
  static int shownStatus = -1;
  const uint32_t now = millis();
  const bool active = ready();
  const uint32_t currentSession = session.load();
  if (seenSession != currentSession || active != wasReady) {
    gestures.reset();
    click = 0;
    waitForLift = true;
    seenSession = currentSession;
    wasReady = active;
    if (active) sendReport(0);
    drawButtons();
  }
  const int status = active ? 2 : connected ? 1 : 0;
  if (status != shownStatus) {
    gfx->fillRect(12, 58, 256, 18, BG);
    text(14, 58, status == 2 ? "CONNECTED" : status == 1 ? "SETTING UP..." : "PAIR / RECONNECT",
         status == 2 ? RGB565(60, 230, 150) : CYAN, 2);
    shownStatus = status;
  }
  if (click && uint32_t(now - clickAt) >= 30) {
    click = 0;
    sendReport(gestures.dragging ? 1 : 0);
  }
  bool down;
  int x, y;
  if (readTouch(down, x, y)) {
    lastGoodTouch = now;
    if (!down) waitForLift = false;
    if (active && !waitForLift) {
      const trackpad::Event event = gestures.update(down, x, y, now);
      if (event.click && !gestures.dragging && !click) {
        click = event.click;
        clickAt = now;
      }
      if (event.dx || event.dy || event.wheel || event.click || event.dragChanged) {
        sendReport((gestures.dragging ? 1 : 0) | click, event.dx, event.dy, event.wheel);
      }
      if (event.dragChanged) drawButtons();
    }
  } else if (uint32_t(now - lastGoodTouch) > 150) {
    // A failed touch controller must not leave the computer's button held.
    const bool held = gestures.dragging || click;
    gestures.reset();
    click = 0;
    waitForLift = true;
    if (held) {
      sendReport(0);
      drawButtons();
    }
  }
  delay(8);
}
