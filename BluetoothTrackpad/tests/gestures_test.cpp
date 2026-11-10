#include "../gestures.h"
#include <assert.h>
#include <stdio.h>

using namespace trackpad;

int main() {
  Gestures g;
  g.update(true, 100, 180, 0);
  Event e = g.update(true, 110, 175, 8);
  assert(e.dx == 20 && e.dy == -10 && !e.click);
  assert(!g.update(false, 0, 0, 100).click); // A swipe is not a tap.
  g.update(true, 100, 180, 200);
  assert(g.update(false, 0, 0, 300).click == 1);
  g.update(true, 100, 180, 400);
  assert(!g.update(false, 0, 0, 700).click); // Long press is not a tap.

  g.update(true, 40, 390, 800);
  assert(g.update(false, 0, 0, 900).click == 1);
  g.update(true, 120, 390, 1000);
  assert(g.update(false, 0, 0, 1100).click == 2);
  g.update(true, 220, 390, 1200);
  assert(g.update(false, 0, 0, 1300).dragChanged && g.dragging);
  g.update(true, 100, 180, 1400);
  assert(g.update(true, 110, 190, 1410).dx == 20 && g.dragging);
  g.update(false, 0, 0, 1450);
  assert(g.dragging); // Finger lift keeps drag latched for repositioning.
  g.update(true, 220, 390, 1500);
  assert(g.update(false, 0, 0, 1600).dragChanged && !g.dragging);

  g.update(true, 250, 200, 1700);
  assert(!g.update(true, 250, 210, 1710).wheel);
  assert(g.update(true, 250, 220, 1720).wheel == -1);
  assert(g.update(true, 250, 200, 1730).wheel == 1);
  assert(!g.update(false, 0, 0, 1800).click);

  // Crossing into a different region cancels the gesture until finger lift.
  g.update(true, 100, 340, 1900);
  assert(!g.update(true, 100, 380, 1910).dy);
  assert(!g.update(true, 100, 330, 1920).dy);
  assert(!g.update(false, 0, 0, 1950).click);
  // Crossing out of a button cannot click a different button.
  g.update(true, 40, 390, 2000);
  g.update(true, 120, 390, 2010);
  assert(!g.update(false, 0, 0, 2020).click);

  g.update(true, 100, 180, 2100);
  assert(!g.update(true, 200, 280, 2110).dx); // Reject implausible jumps.
  assert(g.update(true, 202, 282, 2120).dx == 4);
  g.reset();
  assert(!g.dragging && !g.update(false, 0, 0, 2130).click);

  // Unsigned elapsed-time arithmetic survives millis() rollover.
  g.update(true, 100, 180, UINT32_MAX - 50);
  assert(g.update(false, 0, 0, 50).click == 1);
  assert(zoneAt(-1, 200) == Zone::NONE);
  assert(zoneAt(280, 200) == Zone::NONE);
  assert(zoneAt(100, 100) == Zone::NONE);
  puts("Trackpad gesture tests passed");
}
