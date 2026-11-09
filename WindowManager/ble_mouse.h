#ifndef WINDOW_MANAGER_BLE_MOUSE_H
#define WINDOW_MANAGER_BLE_MOUSE_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <Preferences.h>
#include <freertos/queue.h>
#include <host/ble_store.h>
#include <mbedtls/aes.h>
#include "mouse_hid.h"
#include "mouse_privacy.h"
#include "peer_store.h"

#if !defined(CONFIG_NIMBLE_ENABLED)
#error This sketch uses the NimBLE stack supplied by ESP32-S3 core 3.3.12.
#endif

// The proven BluetoothMouse transport, without paint/cursor/rendering logic.
// Self-contained so arduino-cli can build this sketch independently.
namespace wm_ble {
using namespace bluetooth_mouse;
enum class Phase { PAIRING, RECONNECTING, CONNECTING, CONNECTED, ERROR };
enum class Action { NONE, PAIR, SELECT, CANCEL, RESCAN, FORGET };
struct Command { Action action = Action::NONE; Peer peer; };
struct Input { Motion motion; bool reset = false; };
struct State {
  Phase phase = Phase::PAIRING;
  bool pairing = true, scanning = false, hasSaved = false;
  char message[40] = "Starting Bluetooth";
  char name[24] = "No mouse connected";
  Peer peers[8];
  uint8_t count = 0;
  uint32_t packets = 0;
  char identityAddress[18] = "";
  uint8_t identityType = 0;
  int authResult = 0;
  bool dialing = false;
  uint32_t dialStarted = 0;
  uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
};
static const uint32_t CONNECT_TIMEOUT_MS = 8000;
static State state;
static Command pending;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static StaticQueue_t queueStorage;
alignas(4) static uint8_t queueBytes[256 * sizeof(Input)];
static QueueHandle_t queue = nullptr;
static bool interrupted = false;
static BLEUUID hidService(uint16_t(0x1812));

inline State snapshot() {
  portENTER_CRITICAL(&mux);
  State result = state;
  portEXIT_CRITICAL(&mux);
  return result;
}
inline void request(Action action, const Peer &peer = Peer()) {
  portENTER_CRITICAL(&mux);
  pending.action = action;
  pending.peer = peer;
  portEXIT_CRITICAL(&mux);
}
inline bool takeCommand(Command &command) {
  portENTER_CRITICAL(&mux);
  command = pending;
  pending.action = Action::NONE;
  portEXIT_CRITICAL(&mux);
  return command.action != Action::NONE;
}
inline bool commandPending() {
  portENTER_CRITICAL(&mux);
  const bool result = pending.action != Action::NONE;
  portEXIT_CRITICAL(&mux);
  return result;
}
inline void enqueue(const Input &input) { // Caller holds mux; never block BLE callbacks.
  if (!queue) return;
  if (interrupted) {
    Input reset;
    reset.reset = true;
    if (xQueueSend(queue, &reset, 0) != pdTRUE) return;
    interrupted = false;
  }
  if (xQueueSend(queue, &input, 0) != pdTRUE) interrupted = true;
}
inline bool nextInput(Input &input) { return queue && xQueueReceive(queue, &input, 0) == pdTRUE; }
inline void setPhase(Phase phase, bool pairing, const char *message) {
  portENTER_CRITICAL(&mux);
  state.phase = phase;
  state.pairing = pairing;
  snprintf(state.message, sizeof(state.message), "%s", message);
  if (phase != Phase::CONNECTED) { Input reset; reset.reset = true; enqueue(reset); }
  portEXIT_CRITICAL(&mux);
  Serial.println(message);
}
inline void onMotion(const Motion &motion) {
  portENTER_CRITICAL(&mux);
  if (state.phase == Phase::CONNECTED) {
    Input input;
    input.motion = motion;
    enqueue(input);
    ++state.packets;
  }
  portEXIT_CRITICAL(&mux);
}
class Security : public BLESecurityCallbacks {
  bool onSecurityRequest() override {
    const Phase phase = snapshot().phase;
    return phase == Phase::CONNECTING || phase == Phase::CONNECTED;
  }
  bool onConfirmPIN(uint32_t) override { return false; }
  void onAuthenticationComplete(ble_gap_conn_desc *connection, int status) override {
    const bool encrypted = !status && connection && connection->sec_state.encrypted;
    Serial.printf("Mouse authentication: %s (status 0x%04x)\n", encrypted ? "OK" : "failed", status);
    const String identity = encrypted ? BLEAddress(connection->peer_id_addr).toString() : String();
    portENTER_CRITICAL(&mux);
    state.authResult = encrypted ? 1 : -1;
    if (encrypted) {
      snprintf(state.identityAddress, sizeof(state.identityAddress), "%s", identity.c_str());
      state.identityType = connection->peer_id_addr.type;
    }
    portEXIT_CRITICAL(&mux);
  }
};
static Security security;

inline bool encryptBlock(const uint8_t *key, const uint8_t *input, uint8_t *output) {
  mbedtls_aes_context context;
  mbedtls_aes_init(&context);
  const bool ok = !mbedtls_aes_setkey_enc(&context, key, 128) &&
      !mbedtls_aes_crypt_ecb(&context, MBEDTLS_AES_ENCRYPT, input, output);
  mbedtls_aes_free(&context);
  return ok;
}
inline bool matchesSaved(const Peer &peer, const SavedPeer &saved) {
  if (!saved.magic) return false;
  if (!strcmp(peer.address, saved.peer.address) && (peer.type & 1) == (saved.peer.type & 1)) return true;
  if (!strcmp(peer.address, saved.identityAddress) && (peer.type & 1) == (saved.identityType & 1)) return true;
  BLEAddress address(String(peer.address), peer.type);
  uint8_t network[6];
  for (unsigned i = 0; i < 6; ++i) network[i] = address.getNative()[5 - i];
  return saved.hasIRK && (peer.type & 1) && resolvesPrivateAddress(network, saved.irk, encryptBlock);
}
inline SavedPeer describeBond(const Peer &peer) {
  SavedPeer saved;
  saved.magic = SAVED_MAGIC;
  saved.peer = peer;
  saved.peer.rssi = 0;
  const State current = snapshot();
  snprintf(saved.identityAddress, sizeof(saved.identityAddress), "%s", current.identityAddress);
  saved.identityType = current.identityType;
  BLEAddress identity(String(saved.identityAddress), saved.identityType);
  ble_store_key_sec key = {};
  key.peer_addr.type = saved.identityType & 1;
  memcpy(key.peer_addr.val, identity.getNative(), 6);
  ble_store_value_sec value = {};
  if (!ble_store_read_peer_sec(&key, &value) && value.irk_present) {
    saved.hasIRK = 1;
    memcpy(saved.irk, value.irk, 16);
  }
  return saved;
}
inline void forgetBond(const SavedPeer &saved) {
  if (!saved.magic) return;
  BLEAddress identity(String(saved.identityAddress), saved.identityType);
  ble_addr_t address = {};
  address.type = saved.identityType & 1;
  memcpy(address.val, identity.getNative(), 6);
  ble_store_util_delete_peer(&address); // Never delete unrelated system bonds.
}
inline void disconnectMouse(BLEClient *client) {
  if (client->isConnected()) client->disconnect();
  while (client->isConnected()) delay(20);
}
inline bool subscribe(BLERemoteCharacteristic *characteristic, notify_callback callback) {
  return characteristic && characteristic->canNotify() && characteristic->subscribe(true, callback, true);
}
inline bool protocolMode(BLERemoteCharacteristic *protocol, uint8_t mode) {
  return protocol && (protocol->canWrite() || protocol->canWriteNoResponse()) &&
      protocol->writeValue(mode, protocol->canWrite());
}
inline bool enableReports(BLERemoteService *service) {
  auto *protocol = service->getCharacteristic(BLEUUID(uint16_t(0x2A4E)));
  auto *mapCharacteristic = service->getCharacteristic(BLEUUID(uint16_t(0x2A4B)));
  ReportMap map;
  if (mapCharacteristic && mapCharacteristic->canRead()) {
    const String bytes = mapCharacteristic->readValue();
    if (parseReportMap(reinterpret_cast<const uint8_t *>(bytes.c_str()), bytes.length(), map) &&
        (!protocol || protocolMode(protocol, 1))) {
      for (const auto &entry : *service->getCharacteristicsByHandle()) {
        auto *characteristic = entry.second;
        if (!characteristic->getUUID().equals(BLEUUID(uint16_t(0x2A4D))) || !characteristic->canNotify()) continue;
        auto *reference = characteristic->getDescriptor(BLEUUID(uint16_t(0x2908)));
        if (!reference) continue;
        const String value = reference->readValue();
        if (value.length() != 2 || uint8_t(value[1]) != 1) continue;
        const Report *report = map.find(uint8_t(value[0]));
        if (!report || !report->isMouse()) continue;
        const Report layout = *report;
        if (subscribe(characteristic, [layout](BLERemoteCharacteristic *, uint8_t *data, size_t size, bool) {
          Motion motion;
          if (decodeReport(layout, data, size, motion)) onMotion(motion);
        })) { Serial.printf("Mouse report ID %u, %u bits\n", layout.id, layout.bits); return true; }
      }
    }
  }
  auto *boot = service->getCharacteristic(BLEUUID(uint16_t(0x2A33)));
  return boot && protocolMode(protocol, 0) && subscribe(boot,
      [](BLERemoteCharacteristic *, uint8_t *data, size_t size, bool) {
        Motion motion;
        if (decodeBoot(data, size, motion)) onMotion(motion);
      });
}
inline const char *connectMouse(BLEClient *client, const Peer &peer, bool pairing) {
  disconnectMouse(client);
  portENTER_CRITICAL(&mux);
  snprintf(state.name, sizeof(state.name), "%s", peer.name);
  state.identityAddress[0] = 0;
  state.authResult = 0;
  portEXIT_CRITICAL(&mux);
  setPhase(Phase::CONNECTING, pairing, "Connecting / securing mouse");
  Serial.printf("Connecting to %s [%s], type %u\n", peer.name, peer.address, peer.type);
  portENTER_CRITICAL(&mux);
  state.dialStarted = millis();
  state.dialing = true;
  portEXIT_CRITICAL(&mux);
  const bool connected = client->connect(BLEAddress(String(peer.address), peer.type), peer.type, CONNECT_TIMEOUT_MS);
  portENTER_CRITICAL(&mux);
  state.dialing = false;
  state.connection = connected ? client->getConnId() : BLE_HS_CONN_HANDLE_NONE;
  portEXIT_CRITICAL(&mux);
  if (!connected) return pairing ? "Connection failed - try again" : "Wake your saved mouse";
  if (commandPending()) return "Cancelled";
  if (!BLESecurity::startSecurity(client->getConnId())) return "Could not start mouse security";
  const uint32_t started = millis();
  while (!snapshot().authResult && client->isConnected() && !commandPending() &&
         uint32_t(millis() - started) < 10000) delay(20);
  if (snapshot().authResult != 1) return "Pairing failed - re-pair mouse";
  if (commandPending()) return "Cancelled";
  client->getServices();
  auto *service = client->getService(hidService);
  if (!service) return "Not a Bluetooth LE HID mouse";
  if (!enableReports(service)) return "Unsupported mouse report";
  if (!client->isConnected()) return "Mouse disconnected during setup";
  client->updateConnParams(6, 12, 0, 400);
  return nullptr;
}
inline Peer discoveredPeer(BLEAdvertisedDevice &device) {
  Peer peer;
  const String name = device.getName();
  snprintf(peer.name, sizeof(peer.name), "%.21s", name.length() ? name.c_str() : "Unnamed HID device");
  snprintf(peer.address, sizeof(peer.address), "%s", device.getAddress().toString().c_str());
  peer.type = device.getAddressType();
  peer.rssi = device.getRSSI();
  return peer;
}
inline void bluetoothTask(void *) {
  if (!BLEDevice::init("Waveshare Mouse Host")) {
    setPhase(Phase::ERROR, true, "Bluetooth failed - reboot");
    vTaskDelete(nullptr);
    return;
  }
  BLEDevice::setSecurityCallbacks(&security);
  BLESecurity::setCapability(ESP_IO_CAP_NONE);
  BLESecurity::setAuthenticationMode(true, false, true);
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setForceAuthentication(false);
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(60);
  BLEClient *client = BLEDevice::createClient();
  Preferences preferences;
  const bool storage = preferences.begin("ble-mouse", false);
  SavedPeer saved;
  if (storage && preferences.getBytesLength("peer") == sizeof(saved)) {
    if (preferences.getBytes("peer", &saved, sizeof(saved)) != sizeof(saved) || !validSavedPeer(saved)) saved = SavedPeer();
  }
  int bonds = 0;
  ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bonds);
  Serial.printf("Mouse storage: %s, saved=%s, bonds=%d\n", storage ? "ready" : "UNAVAILABLE", saved.magic ? "yes" : "no", bonds);
  if (saved.magic) Serial.printf("Restored mouse: %s, identity %s\n", saved.peer.name, saved.identityAddress);
  bool pairing = !saved.magic;
  portENTER_CRITICAL(&mux);
  state.hasSaved = saved.magic;
  if (saved.magic) snprintf(state.name, sizeof(state.name), "%s", saved.peer.name);
  portEXIT_CRITICAL(&mux);
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
        portENTER_CRITICAL(&mux);
        state.count = 0;
        state.hasSaved = saved.magic;
        if (saved.magic) snprintf(state.name, sizeof(state.name), "%s", saved.peer.name);
        portEXIT_CRITICAL(&mux);
        setPhase(pairing ? Phase::PAIRING : Phase::RECONNECTING, pairing,
                 pairing ? "Put mouse in pairing mode" : "Wake your saved mouse");
        if (!pairing) { target = reconnectPeer(saved); connectNext = true; }
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
          if (saved.magic && !sameBond(saved, updated)) forgetBond(saved);
          saved = updated;
        }
        pairing = false;
        portENTER_CRITICAL(&mux);
        state.hasSaved = saved.magic;
        state.packets = 0;
        portEXIT_CRITICAL(&mux);
        setPhase(Phase::CONNECTED, false, stored ? "Drag title bars; + adds windows" : "Connected - not saved");
      }
      continue;
    }
    if (client->isConnected()) { delay(40); continue; }
    if (snapshot().phase == Phase::CONNECTED) {
      pairing = !saved.magic;
      setPhase(pairing ? Phase::PAIRING : Phase::RECONNECTING, pairing, "Mouse asleep / disconnected");
      nextScan = millis();
      if (!pairing) { target = reconnectPeer(saved); connectNext = true; continue; }
    }
    if (int32_t(millis() - nextScan) < 0) { delay(40); continue; }
    portENTER_CRITICAL(&mux);
    state.scanning = true;
    portEXIT_CRITICAL(&mux);
    BLEScanResults *results = scan->start(3, false);
    Peer peers[8];
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
        Serial.printf("HID candidate: %s [%s]\n", peer.name, peer.address);
        uint8_t index = 0;
        while (index < count && peers[index].rssi >= peer.rssi) ++index;
        if (index >= 8) continue;
        if (count < 8) ++count;
        for (uint8_t j = count - 1; j > index; --j) peers[j] = peers[j - 1];
        peers[index] = peer;
      }
    }
    scan->clearResults();
    if (!pairing && !connectNext) { target = reconnectPeer(saved); connectNext = true; }
    portENTER_CRITICAL(&mux);
    state.scanning = false;
    if (pairing) { state.count = count; for (uint8_t i = 0; i < count; ++i) state.peers[i] = peers[i]; }
    portEXIT_CRITICAL(&mux);
    nextScan = millis() + 2000;
  }
}
inline void begin() {
  queue = xQueueCreateStatic(256, sizeof(Input), queueBytes, &queueStorage);
  if (xTaskCreate(bluetoothTask, "desktop-ble", 12288, nullptr, 1, nullptr) != pdPASS)
    setPhase(Phase::ERROR, true, "BLE task failed - reboot");
}
inline void tick() {
  // Core 3.3.12 ignores connect()'s timeout argument in its NimBLE branch.
  portENTER_CRITICAL(&mux);
  const bool cancel = state.dialing && (pending.action != Action::NONE ||
      uint32_t(millis() - state.dialStarted) >= CONNECT_TIMEOUT_MS);
  portEXIT_CRITICAL(&mux);
  if (cancel) ble_gap_conn_cancel();
}
inline void printStatus() {
  const State current = snapshot();
  Serial.printf("Mouse: phase=%u, saved=%u, pairing=%u, name=%s, reports=%lu\n", unsigned(current.phase),
                current.hasSaved, current.pairing, current.name, (unsigned long)current.packets);
}
} // namespace wm_ble
#endif
