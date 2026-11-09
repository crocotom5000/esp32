#include "../mouse_hid.h"
#include "../peer_store.h"
#include "../window_manager.h"
#include <assert.h>
#include <stdio.h>

using namespace bluetooth_mouse;

int main() {
  const uint8_t descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0
  };
  ReportMap map;
  assert(parseReportMap(descriptor, sizeof(descriptor), map));
  assert(map.count == 1 && map.find(1)->bits == 24);
  const uint8_t data[] = {1, 0xF6, 20};
  Motion motion;
  assert(decodeReport(*map.find(1), data, sizeof(data), motion));
  assert(motion.buttons == 1 && motion.dx == -10 && motion.dy == 20);
  assert(!decodeReport(*map.find(1), data, 2, motion));
  assert(!parseReportMap(descriptor, sizeof(descriptor) - 1, map));
  assert(decodeBoot(data, sizeof(data), motion));

  window_manager::Desktop desktop;
  desktop.input(motion.dx, motion.dy, motion.buttons);
  assert(desktop.cursor.x == 130 && desktop.cursor.y == 248);
  desktop.input(0, 127, 0);
  desktop.input(0, 55, 0);
  assert(desktop.cursor.y == 430); // New app must reach beyond the paint app's y=362 bound.
  desktop.input(10, 0, 1);
  assert(desktop.count == 2); // Decoded input can reach/click the dock's plus button.

  SavedPeer saved;
  saved.magic = SAVED_MAGIC;
  snprintf(saved.peer.name, sizeof(saved.peer.name), "Test BLE Mouse");
  snprintf(saved.peer.address, sizeof(saved.peer.address), "70:81:94:0d:fb:aa");
  snprintf(saved.identityAddress, sizeof(saved.identityAddress), "c2:11:22:33:44:55");
  saved.peer.type = saved.identityType = 1;
  saved.hasIRK = 1;
  saved.irk[0] = 17;
  uint8_t flash[88];
  memcpy(flash, &saved, sizeof(flash));
  SavedPeer restored;
  memcpy(&restored, flash, sizeof(restored));
  assert(validSavedPeer(restored) && sameSavedPeer(saved, restored));
  assert(!strcmp(reconnectPeer(restored).address, saved.identityAddress));
  restored.peer.address[0] = '4';
  assert(sameBond(saved, restored));
  restored.identityType = 4;
  assert(!validSavedPeer(restored));
  puts("Mouse decoding and shared pairing tests passed");
}
