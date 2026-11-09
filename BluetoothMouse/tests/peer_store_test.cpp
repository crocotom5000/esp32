#include "../peer_store.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

using namespace bluetooth_mouse;

int main() {
  // Existing installations store this exact 88-byte record. Drawing/UI changes
  // must not invalidate it or reinterpret its addresses and identity key.
  static_assert(offsetof(SavedPeer, peer) == 4, "Legacy peer offset");
  static_assert(offsetof(SavedPeer, identityAddress) == 52, "Legacy identity offset");
  static_assert(offsetof(SavedPeer, irk) == 71, "Legacy identity key offset");
  static_assert(offsetof(SavedPeer, hasIRK) == 87, "Legacy key flag offset");
  SavedPeer saved;
  saved.magic = SAVED_MAGIC;
  snprintf(saved.peer.name, sizeof(saved.peer.name), "Logi POP Mouse");
  snprintf(saved.peer.address, sizeof(saved.peer.address), "70:81:94:0d:fb:aa");
  saved.peer.type = 1;
  snprintf(saved.identityAddress, sizeof(saved.identityAddress), "dc:4a:92:cf:7b:8f");
  saved.identityType = 1;
  saved.hasIRK = 1;
  saved.irk[0] = 42;
  assert(validSavedPeer(saved));

  uint8_t flash[88];
  memcpy(flash, &saved, sizeof(flash));
  SavedPeer loaded;
  memcpy(&loaded, flash, sizeof(loaded));
  assert(validSavedPeer(loaded) && sameSavedPeer(saved, loaded));
  Peer target = reconnectPeer(loaded);
  assert(!strcmp(target.address, saved.identityAddress));
  assert(strcmp(target.address, saved.peer.address)); // Do not dial a stale RPA.
  assert(target.type == 1 && !strcmp(target.name, "Logi POP Mouse"));

  SavedPeer rotated = loaded;
  snprintf(rotated.peer.address, sizeof(rotated.peer.address), "41:22:33:44:55:66");
  assert(sameBond(loaded, rotated)); // Keep the bond when the radio address changes.
  assert(!sameSavedPeer(loaded, rotated));
  rotated = loaded;
  rotated.irk[0] ^= 1;
  assert(!sameSavedPeer(loaded, rotated)); // Refresh new keys even for same identity.
  rotated = loaded;
  rotated.peer.rssi = -99;
  assert(sameSavedPeer(loaded, rotated)); // Do not rewrite flash for signal changes.
  rotated = loaded;
  rotated.identityType = 3;
  assert(sameBond(loaded, rotated)); // Random identity vs resolved random identity.
  rotated.identityType = 0;
  assert(!sameBond(loaded, rotated));
  rotated = loaded;
  rotated.identityAddress[0] = 'e';
  assert(!sameBond(loaded, rotated));

  SavedPeer invalid;
  assert(!validSavedPeer(invalid) && !sameBond(loaded, invalid));
  invalid = loaded;
  invalid.magic ^= 1;
  assert(!validSavedPeer(invalid));
  invalid = loaded;
  memset(invalid.peer.address, 'a', sizeof(invalid.peer.address));
  assert(!validSavedPeer(invalid));
  invalid = loaded;
  invalid.identityAddress[2] = '-';
  assert(!validSavedPeer(invalid));
  invalid = loaded;
  invalid.identityAddress[0] = 'z';
  assert(!validSavedPeer(invalid));
  invalid = loaded;
  invalid.peer.name[23] = 'x';
  assert(!validSavedPeer(invalid));
  invalid = loaded;
  invalid.identityType = 4;
  assert(!validSavedPeer(invalid));
  invalid = loaded;
  invalid.hasIRK = 2;
  assert(!validSavedPeer(invalid));
  puts("Mouse pairing storage tests passed");
}
