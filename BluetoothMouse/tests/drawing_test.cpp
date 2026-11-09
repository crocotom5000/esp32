#include "../drawing.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>

using namespace bluetooth_mouse;

static int pixelCount(const Drawing &drawing) {
  int count = 0;
  for (int y = Drawing::TOP; y <= Drawing::BOTTOM; ++y)
    for (int x = Drawing::LEFT; x <= Drawing::RIGHT; ++x)
      count += drawing.painted(x, y);
  return count;
}

int main() {
  Drawing drawing;
  Pointer pointer;
  assert(pixelCount(drawing) == 0);
  drawing.update(pointer); // Moving with no button does not paint.
  assert(pixelCount(drawing) == 0);

  pointer.buttons = 1;
  drawing.update(pointer);
  pointer.buttons = 0;
  drawing.update(pointer); // A fast click/release is retained before any redraw.
  assert(pixelCount(drawing) == 1 && drawing.painted(140, 228));
  pointer.x = 160;
  drawing.update(pointer);
  assert(pixelCount(drawing) == 1 && !drawing.painted(160, 228));
  pointer.buttons = 1;
  drawing.update(pointer);
  assert(pixelCount(drawing) == 2 && !drawing.painted(150, 228)); // New stroke.
  pointer.x = 170;
  drawing.update(pointer);
  for (int x = 160; x <= 170; ++x) assert(drawing.painted(x, 228));
  assert(pixelCount(drawing) == 12); // One-pixel-wide line, not a brush/ripple.

  const Drawing saved = drawing;
  drawing.clear();
  assert(pixelCount(drawing) == 0 && pixelCount(saved) == 12); // Independent frame snapshot.
  pointer.x = 200;
  drawing.update(pointer); // Clear ends the old stroke even if left remains held.
  assert(pixelCount(drawing) == 1 && drawing.painted(200, 228));
  drawing.endStroke(); // Center/reconnect must also start a fresh stroke.
  pointer.x = 140;
  drawing.update(pointer);
  assert(pixelCount(drawing) == 2 && !drawing.painted(180, 228));

  for (uint8_t buttons : {uint8_t(0), uint8_t(2), uint8_t(4), uint8_t(6)}) {
    drawing.clear();
    pointer.buttons = buttons;
    drawing.update(pointer);
    assert(pixelCount(drawing) == 0);
  }

  // All line octants, both directions, and a stationary held click.
  const int offsets[][2] = {{20, 0}, {0, 20}, {20, 20}, {20, 7}, {7, 20},
      {-20, 7}, {-7, 20}, {-20, -7}, {-7, -20}, {20, -7}, {7, -20}, {0, 0}};
  for (const auto &offset : offsets) {
    for (int reverse = 0; reverse < 2; ++reverse) {
      drawing.clear();
      pointer.x = 140 + (reverse ? offset[0] : 0);
      pointer.y = 228 + (reverse ? offset[1] : 0);
      pointer.buttons = 1;
      drawing.update(pointer);
      pointer.x = 140 + (reverse ? 0 : offset[0]);
      pointer.y = 228 + (reverse ? 0 : offset[1]);
      drawing.update(pointer);
      const int dx = offset[0] < 0 ? -offset[0] : offset[0];
      const int dy = offset[1] < 0 ? -offset[1] : offset[1];
      assert(pixelCount(drawing) == (dx > dy ? dx : dy) + 1);
      assert(drawing.painted(140, 228));
      assert(drawing.painted(140 + offset[0], 228 + offset[1]));
    }
  }

  // Canvas edges, including the final ink-density byte.
  drawing.clear();
  pointer.x = Drawing::LEFT;
  pointer.y = Drawing::TOP;
  drawing.update(pointer);
  pointer.x = Drawing::RIGHT;
  pointer.y = Drawing::BOTTOM;
  drawing.update(pointer);
  assert(drawing.painted(Drawing::LEFT, Drawing::TOP));
  assert(drawing.painted(Drawing::RIGHT, Drawing::BOTTOM));
  assert(pixelCount(drawing) == Drawing::HEIGHT);
  assert(!drawing.painted(Drawing::LEFT - 1, Drawing::TOP));
  assert(!drawing.painted(Drawing::RIGHT + 1, Drawing::BOTTOM));
  pointer.x = INT32_MAX;
  pointer.y = INT32_MIN;
  drawing.update(pointer); // Invalid points cannot scribble outside the bitmap.
  pointer.x = 140;
  pointer.y = 228;
  drawing.clear();
  drawing.update(pointer);
  assert(pixelCount(drawing) == 1);
  puts("Drawing tests passed");
}
