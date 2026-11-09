// Bluetooth LE mouse host — Waveshare ESP32-S3-Touch-AMOLED-1.64.
// CO5300 280x456 QSPI, FT3168 touch on SDA 47 / SCL 48.
// This is a HID CENTRAL, not an ESP32 pretending to be a mouse.
#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <Preferences.h>
#include <Wire.h>
#include <host/ble_store.h>
#include <freertos/queue.h>
#include <mbedtls/aes.h>
#include "drawing.h"
#include "mouse_hid.h"
#include "mouse_privacy.h"
#include "peer_store.h"

#if !defined(CONFIG_NIMBLE_ENABLED)
#error This sketch uses the NimBLE stack supplied by ESP32-S3 core 3.3.12.
#endif

using namespace bluetooth_mouse;

static const int WIDTH = 280, HEIGHT = 456;
static const uint32_t CONNECT_TIMEOUT_MS = 8000;
static const uint32_t FRAME_US = 16667; // Target 60 Hz for small cursor updates.
static const uint8_t MAX_PEERS = 8;
Arduino_DataBus *bus = new Arduino_ESP32QSPI(46, 10, 11, 12, 13, 14);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, 21, 0, WIDTH, HEIGHT, 20, 0);
Arduino_Canvas *frame = new Arduino_Canvas(WIDTH, HEIGHT, nullptr);
static BLEUUID hidService(uint16_t(0x1812));

enum class Phase { PAIRING, RECONNECTING, CONNECTING, CONNECTED, ERROR };
enum class Action { NONE, PAIR, SELECT, CANCEL, RESCAN, FORGET };

struct Command {
  Action action = Action::NONE;
  Peer peer;
};

struct PaintEvent {
  Pointer pointer;
  int32_t wheel = 0;
  bool endStroke = false;
};

struct MouseState {
  Phase phase = Phase::PAIRING;
  bool pairing = true, scanning = false, hasSaved = false;
  char message[40] = "Starting Bluetooth";
  char name[24] = "No mouse connected";
  Peer peers[MAX_PEERS];
  uint8_t count = 0;
  Pointer pointer;
  uint32_t packets = 0;
  char identityAddress[18] = "";
  uint8_t identityType = 0;
  int authResult = 0; // 0 pending, 1 encrypted, -1 failed.
  bool dialing = false;
  uint32_t dialStarted = 0;
  uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
};

static MouseState state;
static Drawing painting;
static const unsigned PAINT_QUEUE_SIZE = 256;
static StaticQueue_t paintQueueStorage;
alignas(4) static uint8_t paintQueueBytes[PAINT_QUEUE_SIZE * sizeof(PaintEvent)];
static QueueHandle_t paintQueue = nullptr;
static bool paintQueueInterrupted = false;
static int32_t smearPending = 0;
static Command pending;
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t renderedFrames = 0, timingStartedAt = 0;
static uint64_t composeTotalUs = 0, transferTotalUs = 0;
static uint32_t fullFrames = 0, partialFrames = 0;
static uint32_t framesPerSecond = 0, reportsPerSecond = 0;
static uint32_t lastComposeUs = 0, lastTransferUs = 0;
static bool screenValid = false;
static MouseState previousScreen;
static uint8_t previousPage = 0;
static uint16_t inkColors[256];
alignas(16) static uint16_t uploadPixels[280 * 16];

const char *phaseName(Phase phase) {
  switch (phase) {
    case Phase::PAIRING: return "pairing";
    case Phase::RECONNECTING: return "reconnecting";
    case Phase::CONNECTING: return "connecting";
    case Phase::CONNECTED: return "connected";
    default: return "error";
  }
}

MouseState snapshot() {
  portENTER_CRITICAL(&stateMux);
  MouseState copy = state;
  portEXIT_CRITICAL(&stateMux);
  return copy;
}

// Called under stateMux. Only enqueue tiny records from BLE callbacks: drawing
// and whole-canvas effects belong to the UI task, never an interrupt-disabled
// critical section. A full queue breaks the next stroke rather than joining gaps.
void enqueuePaint(const PaintEvent &event) {
  if (!paintQueue) return;
  if (paintQueueInterrupted) {
    PaintEvent end;
    end.endStroke = true;
    if (xQueueSend(paintQueue, &end, 0) != pdTRUE) return;
    paintQueueInterrupted = false;
  }
  if (xQueueSend(paintQueue, &event, 0) != pdTRUE) paintQueueInterrupted = true;
}

void enqueueStrokeEnd() {
  PaintEvent event;
  event.endStroke = true;
  enqueuePaint(event);
}

void processPaintEvents() {
  PaintEvent event;
  for (unsigned i = 0; i < PAINT_QUEUE_SIZE && xQueueReceive(paintQueue, &event, 0) == pdTRUE; ++i) {
    if (event.endStroke) painting.endStroke();
    else {
      painting.update(event.pointer);
      smearPending = clamp(int64_t(smearPending) + event.wheel, -32, 32);
    }
  }
}

void request(Action action, const Peer &peer = Peer()) {
  portENTER_CRITICAL(&stateMux);
  pending.action = action;
  pending.peer = peer; // Copy the displayed device, never just a changing row index.
  portEXIT_CRITICAL(&stateMux);
}

bool takeCommand(Command &command) {
  portENTER_CRITICAL(&stateMux);
  command = pending;
  pending.action = Action::NONE;
  portEXIT_CRITICAL(&stateMux);
  return command.action != Action::NONE;
}

bool commandPending() {
  portENTER_CRITICAL(&stateMux);
  const bool result = pending.action != Action::NONE;
  portEXIT_CRITICAL(&stateMux);
  return result;
}

void setPhase(Phase phase, bool pairing, const char *message) {
  portENTER_CRITICAL(&stateMux);
  state.phase = phase;
  state.pairing = pairing;
  snprintf(state.message, sizeof(state.message), "%s", message);
  if (phase != Phase::CONNECTED) {
    state.pointer.buttons = 0;
    enqueueStrokeEnd();
  }
  portEXIT_CRITICAL(&stateMux);
  Serial.println(message);
}

void onMotion(const Motion &motion) {
  portENTER_CRITICAL(&stateMux);
  if (state.phase == Phase::CONNECTED) {
    state.pointer.apply(motion);
    PaintEvent event;
    event.pointer = state.pointer;
    event.wheel = motion.wheel;
    enqueuePaint(event);
    ++state.packets;
  }
  portEXIT_CRITICAL(&stateMux);
}

class MouseSecurity : public BLESecurityCallbacks {
  bool onSecurityRequest() override {
    const Phase phase = snapshot().phase;
    return phase == Phase::CONNECTING || phase == Phase::CONNECTED;
  }
  bool onConfirmPIN(uint32_t) override { return false; }
  void onAuthenticationComplete(ble_gap_conn_desc *connection, int status) override {
    const bool encrypted = status == 0 && connection && connection->sec_state.encrypted;
    Serial.printf("Mouse authentication: %s (status 0x%04x)\n", encrypted ? "OK" : "failed", status);
    const String identity = encrypted ? BLEAddress(connection->peer_id_addr).toString() : String();
    portENTER_CRITICAL(&stateMux);
    state.authResult = encrypted ? 1 : -1;
    if (encrypted) {
      snprintf(state.identityAddress, sizeof(state.identityAddress), "%s", identity.c_str());
      state.identityType = connection->peer_id_addr.type;
    }
    portEXIT_CRITICAL(&stateMux);
  }
};
static MouseSecurity securityCallbacks;

bool encryptBlock(const uint8_t *key, const uint8_t *input, uint8_t *output) {
  mbedtls_aes_context context;
  mbedtls_aes_init(&context);
  const bool ok = mbedtls_aes_setkey_enc(&context, key, 128) == 0 &&
      mbedtls_aes_crypt_ecb(&context, MBEDTLS_AES_ENCRYPT, input, output) == 0;
  mbedtls_aes_free(&context);
  return ok;
}

bool matchesSaved(const Peer &peer, const SavedPeer &saved) {
  if (!saved.magic) return false;
  if (!strcmp(peer.address, saved.peer.address) && (peer.type & 1) == (saved.peer.type & 1)) return true;
  if (!strcmp(peer.address, saved.identityAddress) && (peer.type & 1) == (saved.identityType & 1)) return true;
  BLEAddress address(String(peer.address), peer.type);
  uint8_t networkOrder[6];
  for (unsigned i = 0; i < 6; ++i) networkOrder[i] = address.getNative()[5 - i];
  return saved.hasIRK && (peer.type & 1) &&
      resolvesPrivateAddress(networkOrder, saved.irk, encryptBlock);
}

SavedPeer describeBond(const Peer &peer) {
  SavedPeer saved;
  saved.magic = SAVED_MAGIC;
  saved.peer = peer;
  saved.peer.rssi = 0;
  const MouseState current = snapshot();
  snprintf(saved.identityAddress, sizeof(saved.identityAddress), "%s", current.identityAddress);
  saved.identityType = current.identityType;
  BLEAddress identity(String(saved.identityAddress), saved.identityType);
  ble_store_key_sec key = {};
  key.peer_addr.type = saved.identityType & 1;
  memcpy(key.peer_addr.val, identity.getNative(), 6); // NimBLE native order.
  ble_store_value_sec value = {};
  if (ble_store_read_peer_sec(&key, &value) == 0 && value.irk_present) {
    saved.hasIRK = 1;
    memcpy(saved.irk, value.irk, 16);
  }
  return saved;
}

void forgetBond(const SavedPeer &saved) {
  if (!saved.magic) return;
  // Remove only this app's selected mouse, never all system Bluetooth bonds.
  BLEAddress identity(String(saved.identityAddress), saved.identityType);
  ble_addr_t address = {};
  address.type = saved.identityType & 1;
  memcpy(address.val, identity.getNative(), 6);
  ble_store_util_delete_peer(&address);
}

void disconnectMouse(BLEClient *client) {
  if (client->isConnected()) client->disconnect();
  while (client->isConnected()) delay(20);
}

bool subscribe(BLERemoteCharacteristic *characteristic, notify_callback callback) {
  // Unlike registerForNotify(), subscribe() reports CCCD write failures.
  return characteristic && characteristic->canNotify() && characteristic->subscribe(true, callback, true);
}

bool protocolMode(BLERemoteCharacteristic *protocol, uint8_t mode) {
  return protocol && (protocol->canWrite() || protocol->canWriteNoResponse()) &&
      protocol->writeValue(mode, protocol->canWrite());
}

bool enableMouseReports(BLERemoteService *service) {
  BLERemoteCharacteristic *protocol = service->getCharacteristic(BLEUUID(uint16_t(0x2A4E)));
  BLERemoteCharacteristic *mapCharacteristic = service->getCharacteristic(BLEUUID(uint16_t(0x2A4B)));
  ReportMap map;
  if (mapCharacteristic && mapCharacteristic->canRead()) {
    const String bytes = mapCharacteristic->readValue();
    if (parseReportMap(reinterpret_cast<const uint8_t *>(bytes.c_str()), bytes.length(), map) &&
        (!protocol || protocolMode(protocol, 1))) {
      // HID has MULTIPLE 0x2A4D characteristics. A UUID-keyed map loses them.
      for (const auto &entry : *service->getCharacteristicsByHandle()) {
        BLERemoteCharacteristic *characteristic = entry.second;
        if (!characteristic->getUUID().equals(BLEUUID(uint16_t(0x2A4D))) || !characteristic->canNotify()) continue;
        BLERemoteDescriptor *reference = characteristic->getDescriptor(BLEUUID(uint16_t(0x2908)));
        if (!reference) continue;
        const String value = reference->readValue();
        if (value.length() != 2 || uint8_t(value[1]) != 1) continue; // Input only.
        const Report *report = map.find(uint8_t(value[0]));
        if (!report || !report->isMouse()) continue;
        const Report layout = *report; // Callback owns its layout across reconnects.
        if (subscribe(characteristic, [layout](BLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
          Motion motion;
          if (decodeReport(layout, data, length, motion)) onMotion(motion);
        })) {
          Serial.printf("Mouse report ID %u, %u bits\n", layout.id, layout.bits);
          return true;
        }
      }
    }
  }
  // Older/simple mice can expose the optional, fixed three-byte Boot protocol.
  BLERemoteCharacteristic *boot = service->getCharacteristic(BLEUUID(uint16_t(0x2A33)));
  if (boot && protocolMode(protocol, 0) && subscribe(boot,
      [](BLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
        Motion motion;
        if (decodeBoot(data, length, motion)) onMotion(motion);
      })) {
    Serial.println("Using Boot Mouse protocol");
    return true;
  }
  return false;
}

const char *connectMouse(BLEClient *client, const Peer &peer, bool pairing) {
  disconnectMouse(client);
  portENTER_CRITICAL(&stateMux);
  snprintf(state.name, sizeof(state.name), "%s", peer.name);
  state.identityAddress[0] = 0;
  state.authResult = 0;
  portEXIT_CRITICAL(&stateMux);
  setPhase(Phase::CONNECTING, pairing, "Connecting / securing mouse");
  Serial.printf("Connecting to %s [%s], type %u\n", peer.name, peer.address, peer.type);
  portENTER_CRITICAL(&stateMux);
  state.dialStarted = millis();
  state.dialing = true;
  portEXIT_CRITICAL(&stateMux);
  const bool connected = client->connect(BLEAddress(String(peer.address), peer.type), peer.type, CONNECT_TIMEOUT_MS);
  portENTER_CRITICAL(&stateMux);
  state.dialing = false;
  state.connection = connected ? client->getConnId() : BLE_HS_CONN_HANDLE_NONE;
  portEXIT_CRITICAL(&stateMux);
  if (!connected) return pairing ? "Connection failed - try again" : "Wake your saved mouse";
  if (commandPending()) return "Cancelled";
  // Initiate asynchronously so Cancel can interrupt pairing and it has a deadline.
  if (!BLESecurity::startSecurity(client->getConnId())) return "Could not start mouse security";
  const uint32_t started = millis();
  while (!snapshot().authResult && client->isConnected() && !commandPending() &&
         uint32_t(millis() - started) < 10000) delay(20);
  if (snapshot().authResult != 1) return "Pairing failed - re-pair mouse";
  if (commandPending()) return "Cancelled";
  client->getServices(); // Rebuild services/characteristics after every connection.
  BLERemoteService *service = client->getService(hidService);
  if (!service) return "Not a Bluetooth LE HID mouse";
  if (!enableMouseReports(service)) return "Unsupported mouse report";
  if (!client->isConnected()) return "Mouse disconnected during setup";
  // Best effort: let the mouse supply fresh reports often enough for 60 Hz.
  // A mouse can reject this or have its own lower report-rate limit.
  const bool requested = client->updateConnParams(6, 12, 0, 400); // 7.5–15 ms.
  Serial.printf("Low-latency mouse interval request: %s\n", requested ? "sent" : "not accepted");
  return nullptr;
}

Peer discoveredPeer(BLEAdvertisedDevice &device) {
  Peer peer;
  const String name = device.getName();
  snprintf(peer.name, sizeof(peer.name), "%.21s", name.length() ? name.c_str() : "Unnamed HID device");
  snprintf(peer.address, sizeof(peer.address), "%s", device.getAddress().toString().c_str());
  peer.type = device.getAddressType();
  peer.rssi = device.getRSSI();
  return peer;
}

void bluetoothTask(void *) {
  if (!BLEDevice::init("Waveshare Mouse Host")) {
    setPhase(Phase::ERROR, true, "Bluetooth failed - reboot");
    vTaskDelete(nullptr);
    return;
  }
  BLEDevice::setSecurityCallbacks(&securityCallbacks);
  BLESecurity::setCapability(ESP_IO_CAP_NONE); // Mice normally use Just Works.
  BLESecurity::setAuthenticationMode(true, false, true); // Bond; allow legacy LE.
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setForceAuthentication(false); // Secure explicitly in worker task.
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(60);
  BLEClient *client = BLEDevice::createClient(); // Reused, not leaked per attempt.
  Preferences preferences;
  const bool storage = preferences.begin("ble-mouse", false);
  SavedPeer saved;
  const size_t storedSize = storage ? preferences.getBytesLength("peer") : 0;
  if (storedSize == sizeof(saved)) {
    if (preferences.getBytes("peer", &saved, sizeof(saved)) != sizeof(saved) || !validSavedPeer(saved)) {
      Serial.println("Saved mouse record is invalid; leaving flash unchanged");
      saved = SavedPeer();
    }
  } else if (storedSize) {
    Serial.printf("Unrecognized saved mouse record: %u bytes\n", unsigned(storedSize));
  }
  int bonds = 0;
  ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bonds);
  Serial.printf("Mouse storage: %s, saved=%s, Bluetooth bonds=%d\n",
                storage ? "ready" : "UNAVAILABLE", saved.magic ? "yes" : "no", bonds);
  if (saved.magic)
    Serial.printf("Restored mouse: %s, identity %s (type %u), private address key=%s\n",
                  saved.peer.name, saved.identityAddress, saved.identityType, saved.hasIRK ? "yes" : "no");
  bool pairing = !saved.magic;
  portENTER_CRITICAL(&stateMux);
  state.hasSaved = saved.magic;
  if (saved.magic) snprintf(state.name, sizeof(state.name), "%s", saved.peer.name);
  portEXIT_CRITICAL(&stateMux);
  setPhase(pairing ? Phase::PAIRING : Phase::RECONNECTING, pairing,
           pairing ? "Put mouse in pairing mode" : "Wake your saved mouse");
  uint32_t nextScan = millis();
  bool connectNext = saved.magic;
  Peer target = saved.magic ? reconnectPeer(saved) : Peer();

  while (true) {
    Command command;
    if (takeCommand(command)) {
      connectNext = false;
      setPhase(Phase::PAIRING, pairing, "Changing connection");
      disconnectMouse(client);
      if (command.action == Action::SELECT) {
        pairing = true;
        target = command.peer;
        connectNext = true;
      } else {
        if (command.action == Action::FORGET) {
          forgetBond(saved);
          if (storage) preferences.remove("peer");
          saved = SavedPeer();
        }
        pairing = command.action != Action::CANCEL || !saved.magic;
        portENTER_CRITICAL(&stateMux);
        state.count = 0;
        state.hasSaved = saved.magic;
        if (saved.magic) snprintf(state.name, sizeof(state.name), "%s", saved.peer.name);
        portEXIT_CRITICAL(&stateMux);
        setPhase(pairing ? Phase::PAIRING : Phase::RECONNECTING, pairing,
                 pairing ? "Put mouse in pairing mode" : "Wake your saved mouse");
        if (!pairing) {
          target = reconnectPeer(saved);
          connectNext = true;
        }
        nextScan = millis();
      }
    }
    if (connectNext) {
      connectNext = false;
      const char *error = connectMouse(client, target, pairing);
      if (commandPending()) { disconnectMouse(client); continue; }
      if (error) {
        setPhase(pairing ? Phase::PAIRING : Phase::RECONNECTING, pairing, error);
        disconnectMouse(client);
        nextScan = millis() + 2500;
      } else {
        const SavedPeer updated = describeBond(target);
        const bool stored = storage && (sameSavedPeer(saved, updated) ||
            preferences.putBytes("peer", &updated, sizeof(updated)) == sizeof(updated));
        if (stored) {
          // A rotated advertising address does not mean this is a new mouse.
          // Never delete the just-restored bond under the same identity.
          if (saved.magic && !sameBond(saved, updated)) forgetBond(saved);
          saved = updated;
        }
        Serial.printf("Mouse selection %s; identity=%s, private address key=%s\n",
                      stored ? "saved" : "NOT SAVED", updated.identityAddress, updated.hasIRK ? "yes" : "no");
        pairing = false;
        portENTER_CRITICAL(&stateMux);
        state.hasSaved = saved.magic;
        state.packets = 0;
        state.pointer.buttons = 0;
        portEXIT_CRITICAL(&stateMux);
        setPhase(Phase::CONNECTED, false, stored ? "Left click / drag to draw" : "Connected - not saved");
      }
      continue;
    }
    if (client->isConnected()) {
      // An idle mouse may send NO reports for hours. Do not time out good links.
      delay(40);
      continue;
    }
    if (snapshot().phase == Phase::CONNECTED) {
      pairing = !saved.magic;
      setPhase(pairing ? Phase::PAIRING : Phase::RECONNECTING, pairing, "Mouse asleep / disconnected");
      nextScan = millis();
      if (!pairing) {
        target = reconnectPeer(saved);
        connectNext = true;
        continue;
      }
    }
    if (int32_t(millis() - nextScan) < 0) { delay(40); continue; }
    portENTER_CRITICAL(&stateMux);
    state.scanning = true;
    portEXIT_CRITICAL(&stateMux);
    BLEScanResults *results = scan->start(3, false);
    Peer peers[MAX_PEERS];
    uint8_t count = 0;
    if (results) {
      for (int i = 0; i < results->getCount(); ++i) {
        BLEAdvertisedDevice device = results->getDevice(i);
        const Peer peer = discoveredPeer(device);
        if (!pairing && matchesSaved(peer, saved) && !connectNext) {
          target = peer;
          snprintf(target.name, sizeof(target.name), "%s", saved.peer.name);
          connectNext = true;
        }
        const bool hid = device.isAdvertisingService(hidService) ||
            (device.haveAppearance() && (device.getAppearance() >> 6) == 15);
        if (!pairing || !hid) continue;
        Serial.printf("HID candidate: %s [%s], RSSI %d\n", peer.name, peer.address, peer.rssi);
        // Keep the eight nearest candidates; insertion also sorts the list.
        uint8_t index = 0;
        while (index < count && peers[index].rssi >= peer.rssi) ++index;
        if (index >= MAX_PEERS) continue;
        if (count < MAX_PEERS) ++count;
        for (uint8_t j = count - 1; j > index; --j) peers[j] = peers[j - 1];
        peers[index] = peer;
      }
    }
    scan->clearResults();
    if (!pairing && !connectNext) {
      target = reconnectPeer(saved);
      connectNext = true; // Also listen for directed reconnect advertisements.
    }
    portENTER_CRITICAL(&stateMux);
    state.scanning = false;
    if (pairing) {
      state.count = count;
      for (uint8_t i = 0; i < count; ++i) state.peers[i] = peers[i];
    }
    portEXIT_CRITICAL(&stateMux);
    nextScan = millis() + 2000;
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

static const uint16_t C_BG = RGB565_BLACK;
static const uint16_t C_TEXT = RGB565(230, 240, 250);
static const uint16_t C_DIM = RGB565(115, 135, 155);
static const uint16_t C_PANEL = RGB565(15, 24, 34);
static const uint16_t C_ACCENT = RGB565(65, 220, 210);
static const uint16_t C_AMBER = RGB565(255, 185, 65);
static uint8_t page = 0;

void text(const char *label, int x, int y, int size, uint16_t color) {
  frame->setTextSize(size);
  frame->setTextColor(color);
  frame->setCursor(x, y);
  frame->print(label);
}

void centered(const char *label, int y, int size, uint16_t color) {
  text(label, (WIDTH - int(strlen(label)) * 6 * size) / 2, y, size, color);
}

void button(const char *label, int x, int y, int width, uint16_t color = C_ACCENT) {
  frame->fillRoundRect(x, y, width, 36, 9, C_PANEL);
  text(label, x + (width - int(strlen(label)) * 6) / 2, y + 14, 1, color);
}

void drawPointer(int x, int y, uint16_t color) {
  frame->fillTriangle(x, y, x, y + 17, x + 11, y + 11, C_BG);
  frame->fillTriangle(x + 1, y + 2, x + 1, y + 14, x + 9, y + 10, color);
  frame->drawLine(x + 5, y + 10, x + 9, y + 18, color);
  frame->drawLine(x + 6, y + 10, x + 10, y + 18, color);
}

void drawFooter(const MouseState &current) {
  frame->fillRect(0, 384, WIDTH, 10, C_BG);
  char detail[40];
  snprintf(detail, sizeof(detail), "%c L   %c M   %c R    Wheel %ld",
           current.pointer.buttons & 1 ? '*' : '-', current.pointer.buttons & 4 ? '*' : '-',
           current.pointer.buttons & 2 ? '*' : '-', (long)current.pointer.wheel);
  centered(detail, 385, 1, C_DIM);
}

void composeScreen(const MouseState &current, const Drawing &canvas) {
  memset(frame->getFramebuffer(), 0, size_t(WIDTH) * HEIGHT * 2);
  centered("MOUSE PAINT", 22, 2, C_TEXT);
  const bool connected = current.phase == Phase::CONNECTED;
  const uint16_t statusColor = connected ? C_ACCENT : C_AMBER;
  if (current.pairing) {
    centered(current.phase == Phase::CONNECTING ? "CONNECTING" : "PAIR A MOUSE", 58, 2, statusColor);
    centered(current.message, 88, 1, C_DIM);
    centered("Hold the mouse's pairing button", 111, 1, C_DIM);
    centered("Then tap its name below", 126, 1, C_DIM);
    if (page * 4 >= current.count) page = 0;
    if (!current.count) {
      centered(current.scanning ? "Scanning for BLE mice..." : "No HID devices found yet", 220, 1, C_TEXT);
      centered("Bluetooth LE only", 244, 1, C_DIM);
    }
    for (uint8_t row = 0; row < 4 && page * 4 + row < current.count; ++row) {
      const Peer &peer = current.peers[page * 4 + row];
      const int y = 153 + row * 51;
      frame->fillRoundRect(12, y, 256, 45, 8, C_PANEL);
      text(peer.name, 22, y + 8, 1, C_TEXT);
      text(peer.address, 22, y + 26, 1, C_DIM);
      char signal[16];
      snprintf(signal, sizeof(signal), "%d dB", peer.rssi);
      text(signal, 220, y + 26, 1, C_DIM);
    }
    centered(current.scanning ? "Scanning..." : "Tap a device to connect", 363, 1, C_DIM);
    button("Rescan", 12, 380, 78);
    button(current.count > 4 ? "Next page" : "BLE only", 101, 380, 78);
    button(current.hasSaved ? "Cancel" : "Pairing", 190, 380, 78);
    if (current.hasSaved) button("Forget saved mouse", 42, 420, 196, C_AMBER);
  } else {
    frame->fillCircle(17, 57, 3, statusColor);
    text(current.name, 28, 53, 1, C_TEXT);
    for (int y = 83; y < 370; y += 24)
      for (int x = 20; x < 268; x += 24) frame->drawPixel(x, y, RGB565(25, 38, 50));
    for (int y = Drawing::TOP; y <= Drawing::BOTTOM; ++y)
      for (int x = Drawing::LEFT; x <= Drawing::RIGHT; ++x)
        if (uint8_t ink = canvas.intensity(x, y))
          frame->getFramebuffer()[y * WIDTH + x] = inkColors[ink];
    if (!connected) {
      centered("RECONNECTING", 178, 2, C_AMBER);
      centered("Wake mouse or tap Pair", 206, 1, C_DIM);
    }
    drawPointer(current.pointer.x, current.pointer.y, connected ? C_ACCENT : C_DIM);
    drawFooter(current);
    centered(current.message, 404, 1, statusColor);
    button("Pair", 12, 420, 78);
    button("Center", 101, 420, 78);
    button("Clear", 190, 420, 78, C_AMBER);
  }
}

void restoreCanvasRect(Rect rect, const Drawing &canvas) {
  uint16_t *pixels = frame->getFramebuffer();
  for (int y = rect.y; y < rect.bottom(); ++y) {
    for (int x = rect.x; x < rect.right(); ++x) {
      const uint8_t ink = canvas.intensity(x, y);
      const bool grid = x >= 20 && x < 268 && y >= 83 && y < 370 &&
          (x - 20) % 24 == 0 && (y - 83) % 24 == 0;
      pixels[y * WIDTH + x] = ink ? inkColors[ink] : grid ? RGB565(25, 38, 50) : C_BG;
    }
  }
}

void pushRect(Rect rect) {
  uint16_t *pixels = frame->getFramebuffer();
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

bool interfaceChanged(const MouseState &current) {
  if (!screenValid || current.phase != previousScreen.phase || current.pairing != previousScreen.pairing ||
      strcmp(current.name, previousScreen.name) || strcmp(current.message, previousScreen.message)) return true;
  return current.pairing && (page != previousPage || current.scanning != previousScreen.scanning ||
      current.hasSaved != previousScreen.hasSaved || current.count != previousScreen.count ||
      memcmp(current.peers, previousScreen.peers, sizeof(current.peers)));
}

bool renderScreen(const MouseState &current, const Drawing &canvas, Rect paintDamage, bool forceFull = false) {
  const uint32_t started = micros();
  bool full = forceFull || interfaceChanged(current);
  Damage damage;
  if (full) {
    composeScreen(current, canvas);
    damage.add(Rect(0, 0, WIDTH, HEIGHT));
  } else if (!current.pairing && current.phase == Phase::CONNECTED) {
    const bool moved = current.pointer.x != previousScreen.pointer.x || current.pointer.y != previousScreen.pointer.y;
    const bool footer = current.pointer.buttons != previousScreen.pointer.buttons ||
        current.pointer.wheel != previousScreen.pointer.wheel;
    if (moved || !paintDamage.empty()) {
      damage.add(cursorRect(previousScreen.pointer.x, previousScreen.pointer.y));
      damage.add(cursorRect(current.pointer.x, current.pointer.y));
    }
    damage.add(paintDamage);
    if (footer) damage.add(Rect(0, 384, WIDTH, 10));
    for (unsigned i = 0; i < damage.count; ++i) restoreCanvasRect(damage.regions[i], canvas);
    if (moved || !paintDamage.empty()) drawPointer(current.pointer.x, current.pointer.y, C_ACCENT);
    if (footer) drawFooter(current);
  } else if (!paintDamage.empty()) {
    // A queued final stroke or touch Clear can arrive just after disconnection.
    composeScreen(current, canvas);
    damage.add(Rect(0, 0, WIDTH, HEIGHT));
    full = true;
  }
  previousScreen = current;
  previousPage = page;
  screenValid = true;
  if (!damage.count) return false; // No transfers at all for an idle cursor.
  const uint32_t composed = micros();
  for (unsigned i = 0; i < damage.count; ++i) pushRect(damage.regions[i]);
  lastComposeUs = uint32_t(composed - started);
  lastTransferUs = uint32_t(micros() - composed);
  if (!renderedFrames) timingStartedAt = millis();
  ++renderedFrames;
  if (full) ++fullFrames;
  else ++partialFrames;
  composeTotalUs += lastComposeUs;
  transferTotalUs += lastTransferUs;
  return true;
}

void benchmarkRenderer() {
  MouseState sample = snapshot();
  sample.phase = Phase::CONNECTED;
  sample.pairing = false;
  uint64_t fullUs = 0, partialUs = 0;
  // Only the displayed test cursor moves; no synthetic paint, pairing changes,
  // or changes to the real mouse position. Real incoming strokes are processed.
  for (int i = 0; i < 15; ++i) {
    processPaintEvents();
    sample.pointer.x = 80 + i;
    sample.pointer.y = 160 + i / 2;
    renderScreen(sample, painting, painting.takeDamage(), true);
    fullUs += lastComposeUs + lastTransferUs;
    delay(1);
  }
  for (int i = 0; i < 120; ++i) {
    processPaintEvents();
    sample.pointer.x = 95 + i % 100;
    sample.pointer.y = 168 + (i % 40) / 2;
    if (renderScreen(sample, painting, painting.takeDamage())) partialUs += lastComposeUs + lastTransferUs;
    delay(1);
  }
  Serial.printf("Renderer benchmark: full-screen %.2f ms/frame; small cursor update %.2f ms/frame; target 60 Hz\n",
                double(fullUs) / 15 / 1000, double(partialUs) / 120 / 1000);
  screenValid = false;
}

void handleTap(uint16_t x, uint16_t y, const MouseState &displayed) {
  if (!displayed.pairing) {
    if (y >= 420 && x < 96) { page = 0; request(Action::PAIR); }
    if (y >= 420 && x >= 96) {
      portENTER_CRITICAL(&stateMux);
      if (x < 185) {
        state.pointer.x = 140;
        state.pointer.y = 228;
        state.pointer.wheel = 0;
        enqueueStrokeEnd(); // Never draw a line across a cursor teleport.
      } else {
        xQueueReset(paintQueue);
        paintQueueInterrupted = true;
      }
      portEXIT_CRITICAL(&stateMux);
      if (x >= 185) {
        painting.clear(); // Large canvas operations run outside stateMux.
        smearPending = 0;
      }
    }
    return;
  }
  if (y >= 420 && displayed.hasSaved) { request(Action::FORGET); return; }
  if (y >= 380 && y < 416) {
    if (x < 96) request(Action::RESCAN);
    else if (x < 185 && displayed.count > 4) page = (page + 1) % 2;
    else if (x >= 185 && displayed.hasSaved) request(Action::CANCEL);
    return;
  }
  if (displayed.phase == Phase::CONNECTING || x < 12 || x >= 268 || y < 153 || y >= 357) return;
  const uint8_t row = (y - 153) / 51, index = page * 4 + row;
  if ((y - 153) % 51 < 45 && index < displayed.count) request(Action::SELECT, displayed.peers[index]);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  if (!gfx->begin()) { Serial.println("Display init failed"); while (true) delay(1000); }
  gfx->setBrightness(200);
  gfx->fillScreen(C_BG);
  if (!psramFound() || !frame->begin() || !frame->getFramebuffer()) {
    gfx->setTextSize(2);
    gfx->setTextColor(C_AMBER);
    gfx->setCursor(12, 180);
    gfx->print("Enable PSRAM");
    while (true) delay(1000);
  }
  frame->setTextWrap(false);
  for (unsigned i = 0; i < 256; ++i)
    inkColors[i] = RGB565(230 * i / 255, 240 * i / 255, 250 * i / 255);
  paintQueue = xQueueCreateStatic(PAINT_QUEUE_SIZE, sizeof(PaintEvent), paintQueueBytes, &paintQueueStorage);
  Wire.begin(47, 48, 400000);
  Wire.setTimeOut(20);
  Wire.beginTransmission(0x38);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();
  if (xTaskCreate(bluetoothTask, "mouse-ble", 12288, nullptr, 1, nullptr) != pdPASS)
    setPhase(Phase::ERROR, true, "BLE task failed - reboot");
}

void loop() {
  static MouseState displayed;
  static bool wasDown = false;
  static uint32_t lastTouch = 0, lastDrawUs = 0, lastTouchPoll = 0;
  static uint32_t sampleAt = 0, sampleFrames = 0, sampleReports = 0;
  const uint32_t now = millis();
  processPaintEvents();
  // Touch needs about 125 Hz, not an I2C round trip on every scheduler tick.
  if (uint32_t(now - lastTouchPoll) >= 8) {
    lastTouchPoll = now;
    uint16_t x = 0, y = 0;
    const bool down = readTouch(x, y);
    if (down && !wasDown && uint32_t(now - lastTouch) >= 300) {
      handleTap(x, y, displayed);
      lastTouch = now;
    }
    wasDown = down;
  }
  // Core 3.3.12's NimBLE connect() ignores its timeout argument. Cancel from
  // the UI task so a sleeping saved mouse cannot block the worker for 30 s.
  portENTER_CRITICAL(&stateMux);
  const bool cancelDial = state.dialing && (pending.action != Action::NONE ||
      uint32_t(millis() - state.dialStarted) >= CONNECT_TIMEOUT_MS);
  portEXIT_CRITICAL(&stateMux);
  if (cancelDial) ble_gap_conn_cancel();
  if (Serial.available()) {
    const char command = Serial.read();
    if (command == 'b') benchmarkRenderer();
    if (command == '?') {
    const MouseState current = snapshot();
    Serial.printf("Mouse status: phase=%s, saved=%u, pairing=%u, mouse=%s, reports=%lu\n",
                  phaseName(current.phase), current.hasSaved, current.pairing,
                  current.name, (unsigned long)current.packets);
    if (renderedFrames)
      Serial.printf("Render: %lu fps, %lu reports/s, compose %.2f ms, transfer %.2f ms (full=%lu, partial=%lu)\n",
                    (unsigned long)framesPerSecond, (unsigned long)reportsPerSecond,
                    double(composeTotalUs) / renderedFrames / 1000,
                    double(transferTotalUs) / renderedFrames / 1000,
                    (unsigned long)fullFrames, (unsigned long)partialFrames);
    ble_gap_conn_desc connection;
    if (current.phase == Phase::CONNECTED && ble_gap_conn_find(current.connection, &connection) == 0)
      Serial.printf("Mouse link: interval %.2f ms, latency %u\n", connection.conn_itvl * 1.25, connection.conn_latency);
    }
  }
  const uint32_t nowUs = micros();
  if (uint32_t(nowUs - lastDrawUs) >= FRAME_US) {
    lastDrawUs = nowUs;
    if (smearPending) {
      const int ticks = clamp(smearPending, -2, 2);
      painting.smear(ticks);
      smearPending -= ticks;
    }
    displayed = snapshot();
    renderScreen(displayed, painting, painting.takeDamage());
  }
  if (uint32_t(now - sampleAt) >= 1000) {
    const MouseState current = snapshot();
    const uint32_t elapsed = now - sampleAt;
    framesPerSecond = uint32_t(uint64_t(renderedFrames - sampleFrames) * 1000 / elapsed);
    reportsPerSecond = current.packets >= sampleReports
        ? uint32_t(uint64_t(current.packets - sampleReports) * 1000 / elapsed) : 0;
    sampleAt = now;
    sampleFrames = renderedFrames;
    sampleReports = current.packets;
  }
  delay(1);
}
