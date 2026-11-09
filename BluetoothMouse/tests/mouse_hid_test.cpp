#include "../mouse_hid.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

using namespace bluetooth_mouse;

static std::vector<uint8_t> descriptor(uint8_t width = 8, bool relative = true,
                                       uint8_t application = 2, uint8_t id = 1) {
  return {
    0x05, 0x01, 0x09, application, 0xA1, 0x01, // Mouse application.
    0x85, id, 0x09, 0x01, 0xA1, 0x00,        // Report ID, Pointer collection.
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05,      // Buttons 1..5.
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x05, 0x81, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x81, 0x03,      // Three padding bits.
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,      // X/Y, signed relative values.
    0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F,
    0x75, width, 0x95, 0x02, 0x81, uint8_t(relative ? 0x06 : 0x02),
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,      // Wheel, always 8 bits.
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0
  };
}

static bool parse(const std::vector<uint8_t> &bytes, ReportMap &map) {
  return parseReportMap(bytes.data(), bytes.size(), map);
}

static void writeBits(std::vector<uint8_t> &bytes, Field field, int32_t value) {
  for (unsigned i = 0; i < field.width; ++i) {
    const unsigned bit = field.bit + i;
    bytes[bit / 8] |= ((uint32_t(value) >> i) & 1) << (bit % 8);
  }
}

static uint32_t randomState = 0x12345678;
static uint32_t randomWord() {
  randomState ^= randomState << 13;
  randomState ^= randomState >> 17;
  randomState ^= randomState << 5;
  return randomState;
}

int main() {
  ReportMap map;
  auto basic = descriptor();
  assert(parse(basic, map));
  assert(map.count == 1 && map.find(1) && !map.find(2));
  const Report report = *map.find(1);
  assert(report.bits == 32 && report.x.bit == 8 && report.y.bit == 16);
  assert(report.wheel.bit == 24 && report.buttons[4].bit == 4);
  const uint8_t packet[] = {0x15, 0xFF, 0x80, 0x7F};
  Motion motion;
  assert(decodeReport(report, packet, sizeof(packet), motion));
  assert(motion.buttons == 0x15 && motion.dx == -1 && motion.dy == -128 && motion.wheel == 127);
  for (size_t length = 0; length < sizeof(packet); ++length)
    assert(!decodeReport(report, packet, length, motion));
  assert(!decodeReport(report, nullptr, 4, motion));
  assert(motion.dx == -1); // Bad packets never clobber a good reading.

  // HOGP carries the report ID in 0x2908, not at the start of this packet.
  auto noID = basic;
  noID.erase(noID.begin() + 6, noID.begin() + 8);
  assert(parse(noID, map) && map.find(0));
  assert(decodeReport(*map.find(0), packet, sizeof(packet), motion) && motion.dx == -1);

  // Packed 12-bit deltas are common on high-resolution Bluetooth mice.
  assert(parse(descriptor(12), map));
  const uint8_t packed[] = {3, 0, 0xF8, 0x7F, 0x80};
  assert(decodeReport(*map.find(1), packed, sizeof(packed), motion));
  assert(motion.dx == -2048 && motion.dy == 2047 && motion.wheel == -128 && motion.buttons == 3);
  assert(parse(descriptor(16), map));
  const uint8_t wide[] = {0, 0, 0x80, 0xFF, 0x7F, 0xFF};
  assert(decodeReport(*map.find(1), wide, sizeof(wide), motion));
  assert(motion.dx == -32768 && motion.dy == 32767 && motion.wheel == -1);

  for (uint8_t width = 1; width <= 16; ++width) {
    auto bytes = descriptor(width);
    // Remove padding so axes start at bit 5, crossing byte boundaries.
    bytes.erase(bytes.begin() + 28, bytes.begin() + 34);
    assert(parse(bytes, map));
    const Report layout = *map.find(1);
    assert(layout.x.bit == 5 && layout.y.bit == 5 + width);
    for (unsigned i = 0; i < 100; ++i) {
      const int32_t dx = int32_t(randomWord() % (1u << width)) - (1 << (width - 1));
      const int32_t dy = int32_t(randomWord() % (1u << width)) - (1 << (width - 1));
      std::vector<uint8_t> data((layout.bits + 7) / 8, 0);
      writeBits(data, layout.x, dx);
      writeBits(data, layout.y, dy);
      writeBits(data, layout.wheel, -7);
      writeBits(data, layout.buttons[0], 1);
      assert(decodeReport(layout, data.data(), data.size(), motion));
      assert(motion.dx == dx && motion.dy == dy && motion.wheel == -7 && motion.buttons == 1);
    }
  }

  // Other applications can share an ID; their input fields still occupy bits.
  std::vector<uint8_t> composite = {
    0x05, 1, 0x09, 6, 0xA1, 1, 0x85, 1,
    0x75, 8, 0x95, 2, 0x81, 3, 0xC0
  };
  composite.insert(composite.end(), basic.begin(), basic.end());
  assert(parse(composite, map));
  assert(map.find(1)->x.bit == 24 && map.find(1)->bits == 48);

  auto multiID = descriptor(8, true, 6, 9); // Keyboard-like report, not a mouse.
  multiID.insert(multiID.end(), basic.begin(), basic.end());
  assert(parse(multiID, map) && map.count == 2);
  assert(!map.find(9)->isMouse() && map.find(1)->isMouse());

  // Global Push/Pop and Feature/Output items must not disturb input offsets.
  auto pushed = basic;
  const uint8_t feature[] = {0xA4, 0x06, 0, 0xFF, 0x75, 16, 0x95, 32, 0xB1, 2, 0x91, 2, 0xB4};
  pushed.insert(pushed.begin() + 34, feature, feature + sizeof(feature));
  assert(parse(pushed, map) && map.find(1)->bits == 32 && map.find(1)->x.bit == 8);

  assert(!parse(descriptor(8, false), map)); // Absolute/touch input is not a mouse.
  assert(!parse(descriptor(8, true, 6), map));
  assert(!parse(descriptor(0), map));
  assert(!parse(descriptor(33), map));
  assert(!parse(descriptor(8, true, 2, 0), map));
  assert(!parseReportMap(nullptr, 10, map));
  assert(!parseReportMap(basic.data(), 0, map));
  for (size_t length = 0; length < basic.size(); ++length)
    assert(!parseReportMap(basic.data(), length, map));
  assert(parse(basic, map));
  auto malformed = basic;
  malformed.push_back(0xB4); // Pop underflow.
  assert(!parse(malformed, map) && map.count == 1 && map.find(1)->bits == 32);
  malformed = {0xFE, 0, 0};
  assert(!parse(malformed, map));
  malformed = basic;
  malformed.insert(malformed.begin(), {0x75, 32, 0x96, 0xFF, 0xFF, 0x81, 3});
  assert(!parse(malformed, map)); // Size/count multiplication stays bounded.

  const uint8_t boot[] = {7, 0x80, 0x7F, 0xFF};
  assert(decodeBoot(boot, 3, motion) && motion.dx == -128 && motion.dy == 127 && motion.wheel == 0);
  assert(decodeBoot(boot, 4, motion) && motion.wheel == -1 && motion.buttons == 7);
  assert(!decodeBoot(boot, 2, motion) && !decodeBoot(nullptr, 3, motion));

  Pointer pointer;
  motion.dx = INT32_MAX;
  motion.dy = INT32_MIN;
  motion.wheel = INT32_MAX;
  motion.buttons = 1;
  pointer.apply(motion);
  assert(pointer.x == 260 && pointer.y == 72 && pointer.wheel == INT32_MAX && pointer.buttons == 1);
  pointer.apply(motion);
  assert(pointer.wheel == INT32_MAX);
  motion.dx = INT32_MIN;
  motion.dy = INT32_MAX;
  motion.wheel = INT32_MIN;
  motion.buttons = 0;
  pointer.apply(motion);
  pointer.apply(motion);
  assert(pointer.x == 8 && pointer.y == 362 && pointer.wheel == INT32_MIN && pointer.buttons == 0);

  // Exercise malformed descriptors and mutations under ASan/UBSan.
  for (unsigned iteration = 0; iteration < 20000; ++iteration) {
    std::vector<uint8_t> bytes;
    if (iteration & 1) {
      bytes = basic;
      bytes[randomWord() % bytes.size()] = uint8_t(randomWord());
    } else {
      bytes.resize(randomWord() % 128);
      for (uint8_t &byte : bytes) byte = uint8_t(randomWord());
    }
    ReportMap fuzzed;
    if (!parse(bytes, fuzzed)) continue;
    for (uint8_t i = 0; i < fuzzed.count; ++i) {
      const Report &layout = fuzzed.reports[i];
      std::vector<uint8_t> data((layout.bits + 7) / 8, 0xFF);
      decodeReport(layout, data.data(), data.size(), motion);
    }
  }
  puts("Mouse HID tests passed");
}
