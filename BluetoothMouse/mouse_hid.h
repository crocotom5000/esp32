#ifndef BLUETOOTH_MOUSE_HID_H
#define BLUETOOTH_MOUSE_HID_H

#include <stddef.h>
#include <stdint.h>

namespace bluetooth_mouse {

struct Field {
  uint16_t bit = 0;
  uint8_t width = 0;
};

struct Report {
  uint8_t id = 0;
  uint16_t bits = 0;
  Field x, y, wheel, buttons[5];
  bool isMouse() const { return x.width && y.width; }
};

struct ReportMap {
  Report reports[8];
  uint8_t count = 0;

  const Report *find(uint8_t id) const {
    for (uint8_t i = 0; i < count; ++i)
      if (reports[i].id == id) return &reports[i];
    return nullptr;
  }
};

struct Globals {
  uint32_t page = 0, size = 0, count = 0;
  int32_t minimum = 0;
  uint8_t id = 0;
};

struct Usages {
  uint32_t values[32] = {};
  uint8_t count = 0;
  uint32_t first = 0, last = 0;
  bool range = false;

  uint32_t at(uint32_t i) const {
    if (i < count) return values[i];
    if (range && last >= first && i - count <= last - first)
      return first + i - count;
    return count ? values[count - 1] : 0;
  }
};

inline int32_t signedValue(uint32_t value, unsigned width) {
  // Use wide arithmetic rather than implementation-defined signed shifts/casts.
  return (value & (uint32_t(1) << (width - 1)))
      ? int32_t(int64_t(value) - (int64_t(1) << width)) : int32_t(value);
}

// Parse the standard HID short items. Track padding, report IDs, collections,
// and global Push/Pop; never mistake keyboard/vendor/absolute axes for a mouse.
// Limits keep untrusted descriptors bounded. On failure, leave output unchanged.
inline bool parseReportMap(const uint8_t *data, size_t length, ReportMap &output) {
  if (!data || !length || length > 4096) return false;
  ReportMap map;
  Globals global, stack[4];
  Usages local;
  uint8_t stackSize = 0, depth = 0;
  bool mouseCollection[16] = {};
  for (size_t pos = 0; pos < length;) {
    const uint8_t prefix = data[pos++];
    if (prefix == 0xFE) return false; // Long items are not supported.
    const unsigned size = (prefix & 3) == 3 ? 4 : prefix & 3;
    const unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
    if (length - pos < size) return false;
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= uint32_t(data[pos++]) << (8 * i);
    if (type == 1) {
      switch (tag) {
        case 0: global.page = value; break;
        case 1: global.minimum = size ? signedValue(value, size * 8) : 0; break;
        case 7: global.size = value; break;
        case 8:
          if (!value || value > 255) return false;
          global.id = uint8_t(value);
          break;
        case 9: global.count = value; break;
        case 10:
          if (stackSize == 4) return false;
          stack[stackSize++] = global;
          break;
        case 11:
          if (!stackSize) return false;
          global = stack[--stackSize];
          break;
        default: break;
      }
    } else if (type == 2) {
      const uint32_t usage = size == 4 ? value : (global.page << 16) | value;
      if (tag == 0) {
        if (local.count == 32) return false;
        local.values[local.count++] = usage;
      } else if (tag == 1) {
        local.first = usage;
        local.range = true;
      } else if (tag == 2) {
        local.last = usage;
      } else if (tag == 10) {
        return false; // Delimited alternative usage sets are not supported.
      }
    } else if (type == 0) {
      if (tag == 10) { // Collection
        if (depth == 16) return false;
        const bool parent = depth && mouseCollection[depth - 1];
        mouseCollection[depth++] = parent || (local.at(0) == 0x00010002 && value == 1);
      } else if (tag == 12) { // End Collection
        if (!depth) return false;
        --depth;
      } else if (tag == 8) { // Input (Output/Feature have separate bit offsets).
        if (!global.size || global.size > 32 || !global.count || global.count > 512)
          return false;
        Report *report = nullptr;
        for (uint8_t i = 0; i < map.count; ++i)
          if (map.reports[i].id == global.id) report = &map.reports[i];
        if (!report) {
          if (map.count == 8) return false;
          report = &map.reports[map.count++];
          report->id = global.id;
        }
        const uint32_t end = report->bits + global.size * global.count;
        if (end > 4096) return false;
        if (depth && mouseCollection[depth - 1] && !(value & 1) && (value & 2)) {
          for (uint32_t i = 0; i < global.count; ++i) {
            const uint32_t usage = local.at(i);
            Field *field = nullptr;
            if ((value & 4) && global.minimum < 0) {
              if (usage == 0x00010030) field = &report->x;
              if (usage == 0x00010031) field = &report->y;
              if (usage == 0x00010038) field = &report->wheel;
            }
            if (usage >= 0x00090001 && usage <= 0x00090005)
              field = &report->buttons[usage - 0x00090001];
            if (field) {
              if (global.size > 16 || field->width) return false;
              field->bit = uint16_t(report->bits + i * global.size);
              field->width = uint8_t(global.size);
            }
          }
        }
        report->bits = uint16_t(end);
      }
      local = Usages(); // Local items expire after every main item.
    }
  }
  if (depth || stackSize) return false;
  bool found = false, zeroID = false, nonzeroID = false;
  for (uint8_t i = 0; i < map.count; ++i) {
    found |= map.reports[i].isMouse();
    zeroID |= map.reports[i].id == 0;
    nonzeroID |= map.reports[i].id != 0;
  }
  if (!found || (zeroID && nonzeroID)) return false;
  output = map;
  return true;
}

struct Motion {
  int32_t dx = 0, dy = 0, wheel = 0;
  uint8_t buttons = 0;
};

inline uint32_t readBits(const uint8_t *data, Field field) {
  uint32_t result = 0;
  for (uint8_t i = 0; i < field.width; ++i)
    result |= uint32_t((data[(field.bit + i) / 8] >> ((field.bit + i) % 8)) & 1) << i;
  return result;
}

// HOGP report IDs live in the Report Reference descriptor, NOT the payload.
inline bool decodeReport(const Report &report, const uint8_t *data, size_t length, Motion &output) {
  if (!report.isMouse() || !data || length < (size_t(report.bits) + 7) / 8) return false;
  Motion motion;
  motion.dx = signedValue(readBits(data, report.x), report.x.width);
  motion.dy = signedValue(readBits(data, report.y), report.y.width);
  if (report.wheel.width)
    motion.wheel = signedValue(readBits(data, report.wheel), report.wheel.width);
  for (uint8_t i = 0; i < 5; ++i)
    if (readBits(data, report.buttons[i])) motion.buttons |= 1 << i;
  output = motion;
  return true;
}

inline bool decodeBoot(const uint8_t *data, size_t length, Motion &output) {
  if (!data || length < 3) return false;
  Motion motion;
  motion.buttons = data[0] & 7;
  motion.dx = signedValue(data[1], 8);
  motion.dy = signedValue(data[2], 8);
  // Boot Mouse Input is three bytes. Some mice append a wheel byte.
  if (length >= 4) motion.wheel = signedValue(data[3], 8);
  output = motion;
  return true;
}

inline int32_t clamp(int64_t value, int32_t low, int32_t high) {
  return value < low ? low : value > high ? high : int32_t(value);
}

struct Pointer {
  int32_t x = 140, y = 228, wheel = 0;
  uint8_t buttons = 0;

  void apply(const Motion &motion) {
    x = clamp(int64_t(x) + motion.dx, 8, 260);
    y = clamp(int64_t(y) + motion.dy, 72, 362);
    wheel = clamp(int64_t(wheel) + motion.wheel, INT32_MIN, INT32_MAX);
    buttons = motion.buttons;
  }
};

} // namespace bluetooth_mouse
#endif
