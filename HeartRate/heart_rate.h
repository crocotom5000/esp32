#pragma once

#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

namespace heart_rate {

struct Measurement {
  uint16_t bpm = 0;
  bool contactSupported = false;
  bool contactDetected = false;
  bool hasRR = false;
  uint16_t rrMs = 0; // Last beat-to-beat interval, not an HRV estimate.
};

inline uint16_t littleEndian16(const uint8_t *data) {
  return uint16_t(data[0]) | (uint16_t(data[1]) << 8);
}

// Bluetooth Heart Rate Measurement (0x2A37). Validate optional fields before
// publishing anything, so a truncated notification cannot replace good data.
inline bool parseMeasurement(const uint8_t *data, size_t length, Measurement &out) {
  if (!data || length < 2) return false;
  Measurement value;
  const uint8_t flags = data[0];
  const size_t bpmBytes = (flags & 0x01) ? 2 : 1;
  if (length < 1 + bpmBytes) return false;
  value.bpm = bpmBytes == 2 ? littleEndian16(data + 1) : data[1];
  value.contactSupported = flags & 0x04;
  value.contactDetected = value.contactSupported && (flags & 0x02);
  size_t offset = 1 + bpmBytes;
  if (flags & 0x08) { // Energy expended (kJ), not used by this display.
    if (length - offset < 2) return false;
    offset += 2;
  }
  if (flags & 0x10) { // One or more RR intervals, in units of 1/1024 second.
    if (length - offset < 2 || (length - offset) % 2 != 0) return false;
    const uint16_t rr = littleEndian16(data + length - 2);
    value.hasRR = true;
    value.rrMs = (uint32_t(rr) * 1000 + 512) / 1024;
  } else if (length != offset) {
    return false;
  }
  out = value;
  return true;
}

inline bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return uint32_t(now - since) >= interval;
}

inline bool containsIgnoreCase(const char *text, const char *needle) {
  if (!text || !needle) return false;
  for (; *text; ++text) {
    size_t i = 0;
    while (needle[i] && text[i] &&
           tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i])) ++i;
    if (!needle[i]) return true;
  }
  return false;
}

inline bool isWahooName(const char *name) {
  return containsIgnoreCase(name, "wahoo") || containsIgnoreCase(name, "tickr") ||
         containsIgnoreCase(name, "trackr");
}

inline bool usable(const Measurement &value) {
  return value.bpm != 0 && (!value.contactSupported || value.contactDetected);
}

} // namespace heart_rate
