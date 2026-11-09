#include "../drawing.h"
#include <assert.h>
#include <stdio.h>

using namespace bluetooth_mouse;

static void dot(Drawing &drawing, int x, int y) {
  Pointer pointer;
  pointer.x = x;
  pointer.y = y;
  pointer.buttons = 1;
  drawing.endStroke();
  drawing.update(pointer);
}

int main() {
  Drawing drawing;
  assert(drawing.takeDamage().empty());
  drawing.smear(-1);
  assert(drawing.takeDamage().empty()); // Empty paper has nothing to smear.
  dot(drawing, 140, 228);
  Rect damage = drawing.takeDamage();
  assert(damage.x == 140 && damage.y == 228 && damage.w == 1 && damage.h == 1);
  assert(drawing.intensity(140, 228) == 255);
  drawing.smear(0);
  assert(drawing.takeDamage().empty());

  drawing.smear(-1); // Wheel down pulls ink down, leaving a continuous fading trail.
  const uint8_t gradient[] = {63, 111, 159, 207, 255};
  for (int i = 0; i < 5; ++i) assert(drawing.intensity(140, 228 + i) == gradient[i]);
  assert(!drawing.painted(140, 227) && !drawing.painted(140, 233));
  assert(!drawing.painted(139, 230) && !drawing.painted(141, 230));
  damage = drawing.takeDamage();
  assert(damage.x == 140 && damage.y == 228 && damage.w == 1 && damage.h == 5);

  drawing.clear();
  dot(drawing, 140, 228);
  drawing.smear(1); // Wheel up is exactly the mirrored effect, not an undo.
  for (int i = 0; i < 5; ++i) assert(drawing.intensity(140, 228 - i) == gradient[i]);
  assert(!drawing.painted(140, 229) && !drawing.painted(140, 223));

  drawing.clear();
  dot(drawing, 140, 228);
  dot(drawing, 200, 300);
  drawing.smear(-2); // All ink moves, not just ink around the cursor.
  assert(drawing.intensity(140, 236) == 255 && drawing.intensity(200, 308) == 255);
  assert(drawing.intensity(140, 232) > drawing.intensity(140, 228));
  for (int y = 228; y <= 236; ++y) assert(drawing.painted(140, y));
  drawing.smear(1);
  assert(drawing.intensity(140, 232) == 255 && drawing.intensity(200, 304) == 255);

  // Paint restores full-density white on a faded pixel and marks it dirty.
  drawing.takeDamage();
  dot(drawing, 140, 228);
  assert(drawing.intensity(140, 228) == 255 && !drawing.takeDamage().empty());
  dot(drawing, 140, 228);
  assert(drawing.takeDamage().empty()); // Repainting unchanged ink needs no upload.

  // Smearing interrupts stroke continuity: the next held-button report cannot
  // draw a bridge to the previous, now displaced stroke.
  drawing.clear();
  dot(drawing, 100, 150);
  drawing.smear(-1);
  Pointer pointer;
  pointer.x = 200;
  pointer.y = 150;
  pointer.buttons = 1;
  drawing.update(pointer);
  assert(!drawing.painted(150, 150));

  drawing.clear();
  dot(drawing, Drawing::LEFT, Drawing::TOP);
  dot(drawing, Drawing::RIGHT, Drawing::BOTTOM);
  drawing.smear(INT32_MIN); // Bounded work, no negation overflow or out-of-bounds writes.
  assert(drawing.painted(Drawing::LEFT, Drawing::TOP + 8));
  assert(drawing.painted(Drawing::RIGHT, Drawing::BOTTOM));
  assert(!drawing.painted(Drawing::RIGHT, Drawing::BOTTOM + 1));
  drawing.smear(INT32_MAX);
  assert(drawing.painted(Drawing::LEFT, Drawing::TOP));
  assert(!drawing.painted(Drawing::LEFT, Drawing::TOP - 1));

  // Ink dragged off the paper eventually disappears, rather than wrapping.
  drawing.clear();
  dot(drawing, 140, Drawing::BOTTOM);
  for (int i = 0; i < 12; ++i) drawing.smear(-1);
  for (int y = Drawing::TOP; y <= Drawing::BOTTOM; ++y) assert(!drawing.painted(140, y));
  drawing.takeDamage();
  drawing.smear(-1);
  assert(drawing.takeDamage().empty());
  drawing.clear();
  damage = drawing.takeDamage();
  assert(damage.x == Drawing::LEFT && damage.y == Drawing::TOP &&
         damage.w == Drawing::WIDTH && damage.h == Drawing::HEIGHT);

  // Compare the in-place implementation with an immutable reference for every
  // pixel, in both directions, including already-faded ink and clipped edges.
  const int tickCases[] = {-2, -1, 1, 2};
  for (int ticks : tickCases) {
    drawing.clear();
    for (int y = Drawing::TOP; y <= Drawing::BOTTOM; y += 19)
      for (int x = Drawing::LEFT; x <= Drawing::RIGHT; x += 17) dot(drawing, x, y);
    drawing.smear(-1);
    const Drawing original = drawing;
    drawing.takeDamage();
    drawing.smear(ticks);
    const Rect dirty = drawing.takeDamage();
    const int direction = ticks < 0 ? 1 : -1;
    const int distance = (ticks < 0 ? -ticks : ticks) * 4;
    for (int y = Drawing::TOP; y <= Drawing::BOTTOM; ++y) {
      for (int x = Drawing::LEFT; x <= Drawing::RIGHT; ++x) {
        unsigned expected = 0;
        for (int step = 0; step <= distance; ++step) {
          const unsigned sample = unsigned(original.intensity(x, y - direction * step)) *
              (64 + 192 * step / distance) / 256;
          if (sample > expected) expected = sample;
        }
        assert(drawing.intensity(x, y) == expected);
        if (original.intensity(x, y) != expected)
          assert(x >= dirty.x && x < dirty.right() && y >= dirty.y && y < dirty.bottom());
      }
    }
  }
  puts("Ink smear tests passed");
}
