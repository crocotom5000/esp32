#include "../mouse_privacy.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>

using namespace bluetooth_mouse;

static bool encrypt(const uint8_t *key, const uint8_t *input, uint8_t *output) {
  EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
  assert(context);
  int written = 0, tail = 0;
  const bool ok = EVP_EncryptInit_ex(context, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
      EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
      EVP_EncryptUpdate(context, output, &written, input, 16) == 1 &&
      EVP_EncryptFinal_ex(context, output + written, &tail) == 1;
  EVP_CIPHER_CTX_free(context);
  return ok && written == 16 && tail == 0;
}

int main() {
  // Bluetooth Core ah() sample: IRK ec0234a357c8ad05341010a60a397d9b,
  // prand 708194, hash 0dfbaa. Stack keys are little-endian byte arrays.
  const uint8_t irk[] = {0x9B, 0x7D, 0x39, 0x0A, 0xA6, 0x10, 0x10, 0x34,
                         0x05, 0xAD, 0xC8, 0x57, 0xA3, 0x34, 0x02, 0xEC};
  uint8_t address[] = {0x70, 0x81, 0x94, 0x0D, 0xFB, 0xAA};
  assert(resolvesPrivateAddress(address, irk, encrypt));
  address[5] ^= 1;
  assert(!resolvesPrivateAddress(address, irk, encrypt));
  address[5] ^= 1;
  address[0] |= 0x80; // Static random address is not resolvable private.
  assert(!resolvesPrivateAddress(address, irk, encrypt));
  address[0] &= 0x3F; // Nor is a non-resolvable private address.
  assert(!resolvesPrivateAddress(address, irk, encrypt));
  address[0] = 0x70;
  assert(!resolvesPrivateAddress(address, irk,
      [](const uint8_t *, const uint8_t *, uint8_t *) { return false; }));
  const uint8_t wrongKey[16] = {};
  assert(!resolvesPrivateAddress(address, wrongKey, encrypt));
  puts("Mouse privacy tests passed");
}
