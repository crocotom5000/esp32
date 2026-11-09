#ifndef BLUETOOTH_MOUSE_PEER_STORE_H
#define BLUETOOTH_MOUSE_PEER_STORE_H
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace bluetooth_mouse {
static const uint32_t SAVED_MAGIC = 0x4D4F5501;
struct Peer {
  char name[24] = "";
  char address[18] = "";
  uint8_t type = 0;
  int rssi = 0;
};
// Intentionally compatible with BluetoothMouse's saved selection and bond.
struct SavedPeer {
  uint32_t magic = 0;
  Peer peer;
  char identityAddress[18] = "";
  uint8_t identityType = 0;
  uint8_t irk[16] = {};
  uint8_t hasIRK = 0;
};
static_assert(sizeof(SavedPeer) == 88, "Saved mouse format changed; migration required");
inline bool validAddress(const char *address) {
  if (strnlen(address, 18) != 17) return false;
  for (int i = 0; i < 17; ++i) {
    if (i % 3 == 2) { if (address[i] != ':') return false; }
    else if (!isxdigit(static_cast<unsigned char>(address[i]))) return false;
  }
  return true;
}
inline bool validSavedPeer(const SavedPeer &saved) {
  return saved.magic == SAVED_MAGIC && validAddress(saved.peer.address) &&
      validAddress(saved.identityAddress) && !saved.peer.name[23] &&
      saved.peer.type <= 3 && saved.identityType <= 3 && saved.hasIRK <= 1;
}
inline Peer reconnectPeer(const SavedPeer &saved) {
  Peer target = saved.peer;
  snprintf(target.address, sizeof(target.address), "%s", saved.identityAddress);
  target.type = saved.identityType;
  return target;
}
inline bool sameBond(const SavedPeer &a, const SavedPeer &b) {
  return a.magic && b.magic && !strcmp(a.identityAddress, b.identityAddress) &&
      (a.identityType & 1) == (b.identityType & 1);
}
inline bool sameSavedPeer(const SavedPeer &a, const SavedPeer &b) {
  return sameBond(a, b) && !strcmp(a.peer.name, b.peer.name) &&
      !strcmp(a.peer.address, b.peer.address) && a.peer.type == b.peer.type &&
      a.identityType == b.identityType && a.hasIRK == b.hasIRK && !memcmp(a.irk, b.irk, sizeof(a.irk));
}
} // namespace bluetooth_mouse
#endif
