#ifndef BLUETOOTH_MOUSE_DRAWING_H
#define BLUETOOTH_MOUSE_DRAWING_H

#include <string.h>
#include "mouse_hid.h"
#include "render_regions.h"

namespace bluetooth_mouse {

// An ink-density layer, separate from the cursor and UI framebuffer. Feed it
// every mouse report so a click/release between display frames is still saved.
class Drawing {
public:
  enum {
    LEFT = 8, TOP = 72, RIGHT = 260, BOTTOM = 362,
    WIDTH = RIGHT - LEFT + 1, HEIGHT = BOTTOM - TOP + 1,
    BYTES = WIDTH * HEIGHT
  };

  void update(const Pointer &pointer) {
    if (!(pointer.buttons & 1) || !inside(pointer.x, pointer.y)) {
      endStroke();
      return;
    }
    if (drawing) line(lastX, lastY, pointer.x, pointer.y);
    else paint(pointer.x, pointer.y);
    lastX = pointer.x;
    lastY = pointer.y;
    drawing = true;
  }

  void endStroke() { drawing = false; }

  void clear() {
    memset(pixels, 0, sizeof(pixels));
    inkBounds = Rect();
    changed = Rect(LEFT, TOP, WIDTH, HEIGHT);
    endStroke();
  }

  bool painted(int x, int y) const {
    return intensity(x, y) != 0;
  }

  uint8_t intensity(int x, int y) const {
    return inside(x, y) ? pixels[(y - TOP) * WIDTH + x - LEFT] : 0;
  }

  Rect takeDamage() {
    const Rect result = changed;
    changed = Rect();
    return result;
  }

  // HID wheel values are positive for up, negative for down. Pull the strongest
  // ink four pixels per notch, leaving a progressively fainter wet-ink trail.
  // The UI calls this with at most two notches per frame to bound the work.
  void smear(int ticks) {
    if (!ticks || inkBounds.empty()) return;
    const int direction = ticks < 0 ? 1 : -1;
    const int distance = ticks > 2 || ticks < -2 ? 8 : (ticks < 0 ? -ticks : ticks) * 4;
    Rect affected = inkBounds;
    affected.include(Rect(inkBounds.x, inkBounds.y + direction * distance, inkBounds.w, inkBounds.h));
    const int top = affected.y < TOP ? TOP : affected.y;
    const int bottom = affected.bottom() > BOTTOM + 1 ? BOTTOM + 1 : affected.bottom();
    unsigned weights[9];
    for (int step = 0; step <= distance; ++step) weights[step] = 64 + 192 * step / distance;
    Rect remaining;
    // Traverse against the pull direction: all source samples still contain
    // the original ink, so this needs no second full-size canvas or allocation.
    for (int row = 0; row < bottom - top; ++row) {
      const int y = direction > 0 ? bottom - 1 - row : top + row;
      const uint8_t *sources[9];
      for (int step = 0; step <= distance; ++step) {
        const int sourceY = y - direction * step;
        sources[step] = sourceY >= TOP && sourceY <= BOTTOM
            ? pixels + (sourceY - TOP) * WIDTH + affected.x - LEFT : nullptr;
      }
      uint8_t *destination = pixels + (y - TOP) * WIDTH + affected.x - LEFT;
      int firstChanged = -1, lastChanged = 0, firstInk = -1, lastInk = 0;
      for (int column = 0; column < affected.w; ++column) {
        unsigned ink = 0;
        for (int step = 0; step <= distance; ++step) {
          const unsigned sample = sources[step] ? unsigned(sources[step][column]) * weights[step] / 256 : 0;
          if (sample > ink) ink = sample;
        }
        uint8_t &pixel = destination[column];
        if (pixel != ink) {
          pixel = uint8_t(ink);
          if (firstChanged < 0) firstChanged = column;
          lastChanged = column;
        }
        if (ink) {
          if (firstInk < 0) firstInk = column;
          lastInk = column;
        }
      }
      if (firstChanged >= 0) changed.include(Rect(affected.x + firstChanged, y, lastChanged - firstChanged + 1, 1));
      if (firstInk >= 0) remaining.include(Rect(affected.x + firstInk, y, lastInk - firstInk + 1, 1));
    }
    inkBounds = remaining;
    endStroke(); // Do not bridge a subsequent brush stroke to pre-smear ink.
  }

private:
  uint8_t pixels[BYTES] = {};
  bool drawing = false;
  int lastX = 0, lastY = 0;
  Rect changed, inkBounds;

  static bool inside(int x, int y) {
    return x >= LEFT && x <= RIGHT && y >= TOP && y <= BOTTOM;
  }

  void paint(int x, int y) {
    uint8_t &pixel = pixels[(y - TOP) * WIDTH + x - LEFT];
    if (pixel != 255) {
      pixel = 255;
      changed.include(Rect(x, y, 1, 1));
      inkBounds.include(Rect(x, y, 1, 1));
    }
  }

  // Bresenham joins successive held-button reports without gaps or a wider
  // brush. Endpoints are already inside the canvas, bounding work to 291 pixels.
  void line(int x, int y, int endX, int endY) {
    const int dx = endX > x ? endX - x : x - endX;
    const int dy = -(endY > y ? endY - y : y - endY);
    const int stepX = x < endX ? 1 : -1, stepY = y < endY ? 1 : -1;
    int error = dx + dy;
    while (true) {
      paint(x, y);
      if (x == endX && y == endY) break;
      const int twice = 2 * error;
      if (twice >= dy) { error += dy; x += stepX; }
      if (twice <= dx) { error += dx; y += stepY; }
    }
  }
};

} // namespace bluetooth_mouse
#endif
