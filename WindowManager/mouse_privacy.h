#ifndef BLUETOOTH_MOUSE_PRIVACY_H
#define BLUETOOTH_MOUSE_PRIVACY_H
#include <stdint.h>
#include <string.h>

namespace bluetooth_mouse {
// Address in printed/network order, stack IRK in little-endian order.
template <typename Encrypt>
bool resolvesPrivateAddress(const uint8_t address[6], const uint8_t irk[16], Encrypt encrypt) {
  if ((address[0] & 0xC0) != 0x40) return false;
  uint8_t key[16], input[16] = {}, output[16] = {};
  for (unsigned i = 0; i < 16; ++i) key[i] = irk[15 - i];
  memcpy(input + 13, address, 3);
  return encrypt(key, input, output) && memcmp(output + 13, address + 3, 3) == 0;
}
} // namespace bluetooth_mouse
#endif
