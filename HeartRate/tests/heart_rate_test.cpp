#include "../heart_rate.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>

using namespace heart_rate;

int main() {
  Measurement value;
  const uint8_t basic[] = {0x00, 72};
  assert(parseMeasurement(basic, sizeof(basic), value));
  assert(value.bpm == 72 && !value.contactSupported && !value.hasRR);
  assert(usable(value));

  const uint8_t wide[] = {0x07, 0x2C, 0x01};
  assert(parseMeasurement(wide, sizeof(wide), value));
  assert(value.bpm == 300 && value.contactSupported && value.contactDetected);

  const uint8_t noContact[] = {0x04, 90};
  assert(parseMeasurement(noContact, sizeof(noContact), value));
  assert(!value.contactDetected && !usable(value));
  const uint8_t unsupportedContact[] = {0x02, 90};
  assert(parseMeasurement(unsupportedContact, sizeof(unsupportedContact), value));
  assert(!value.contactSupported && !value.contactDetected && usable(value));
  const uint8_t zero[] = {0x00, 0};
  assert(parseMeasurement(zero, sizeof(zero), value) && !usable(value));

  // Energy + multiple RR values; the final interval is displayed.
  const uint8_t optional[] = {0x1E, 80, 10, 0, 0x00, 0x04, 0x00, 0x03};
  assert(parseMeasurement(optional, sizeof(optional), value));
  assert(value.bpm == 80 && value.hasRR && value.rrMs == 750);
  const uint8_t allWide[] = {0x1F, 0x01, 0x01, 10, 0, 0x00, 0x04};
  assert(parseMeasurement(allWide, sizeof(allWide), value));
  assert(value.bpm == 257 && value.rrMs == 1000);

  assert(!parseMeasurement(nullptr, 4, value));
  for (size_t n = 0; n < sizeof(wide); ++n)
    assert(!parseMeasurement(wide, n, value));
  for (size_t n : {size_t(2), size_t(3), size_t(4), size_t(5), size_t(7)})
    assert(!parseMeasurement(optional, n, value));
  const uint8_t trailing[] = {0, 72, 1};
  assert(!parseMeasurement(trailing, sizeof(trailing), value));
  assert(value.bpm == 257); // Invalid packets do not clobber the previous value.

  assert(isWahooName("Wahoo TICKR 1234"));
  assert(isWahooName("tickr X"));
  assert(isWahooName("TRACKR HEART RATE"));
  assert(!isWahooName("Polar H10"));
  assert(!isWahooName(""));
  assert(!isWahooName(nullptr));
  assert(!elapsed(4999, 0, 5000));
  assert(elapsed(5000, 0, 5000));
  assert(!elapsed(3, UINT32_MAX - 5, 10));
  assert(elapsed(4, UINT32_MAX - 5, 10));
  puts("Heart rate tests passed");
}
