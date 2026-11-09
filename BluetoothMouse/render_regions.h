#ifndef BLUETOOTH_MOUSE_RENDER_REGIONS_H
#define BLUETOOTH_MOUSE_RENDER_REGIONS_H

#include <stdint.h>
#include <string.h>

namespace bluetooth_mouse {

struct Rect {
  int x, y, w, h;
  Rect(int x = 0, int y = 0, int w = 0, int h = 0) : x(x), y(y), w(w), h(h) {}
  bool empty() const { return w <= 0 || h <= 0; }
  int right() const { return x + w; }
  int bottom() const { return y + h; }
  int area() const { return empty() ? 0 : w * h; }

  bool overlaps(const Rect &other) const {
    return !empty() && !other.empty() && x < other.right() && other.x < right() &&
        y < other.bottom() && other.y < bottom();
  }

  void include(const Rect &other) {
    if (other.empty()) return;
    if (empty()) { *this = other; return; }
    const int endX = right() > other.right() ? right() : other.right();
    const int endY = bottom() > other.bottom() ? bottom() : other.bottom();
    x = x < other.x ? x : other.x;
    y = y < other.y ? y : other.y;
    w = endX - x;
    h = endY - y;
  }

  Rect clipped(int width, int height) const {
    const int left = x < 0 ? 0 : x, top = y < 0 ? 0 : y;
    const int endX = right() > width ? width : right();
    const int endY = bottom() > height ? height : bottom();
    return Rect(left, top, endX - left, endY - top);
  }
};

inline Rect cursorRect(int x, int y) { return Rect(x, y, 12, 19); }

// CO5300 column/row starts must be even and inclusive ends odd. Expand before
// restoring/composing the image, not just before uploading, so the extra border
// pixels contain the correct background/ink instead of stale cursor fragments.
inline Rect panelRect(Rect rect) {
  rect = rect.clipped(280, 456);
  if (rect.empty()) return Rect();
  const int left = rect.x & ~1, top = rect.y & ~1;
  return Rect(left, top, ((rect.right() + 1) & ~1) - left, ((rect.bottom() + 1) & ~1) - top);
}

inline int stripRows(int width, int pixelCapacity) {
  return (pixelCapacity / width) & ~1;
}

// Keep distant old/new cursor positions separate instead of uploading the
// large, unchanged rectangle between them. Merge overlaps transitively.
class Damage {
public:
  Rect regions[4];
  unsigned count = 0;

  void add(Rect rect) {
    rect = panelRect(rect);
    if (rect.empty()) return;
    for (unsigned i = 0; i < count;) {
      if (!rect.overlaps(regions[i])) { ++i; continue; }
      rect.include(regions[i]);
      regions[i] = regions[--count];
      i = 0;
    }
    if (count == 4) {
      for (unsigned i = 0; i < count; ++i) rect.include(regions[i]);
      count = 0;
    }
    regions[count++] = rect;
  }
};

// Crops need tightly packed rows: passing an interior pointer from the full
// framebuffer directly to draw16bitRGBBitmap would use the wrong row stride.
inline void packRect(const uint16_t *source, int stride, Rect rect, uint16_t *destination) {
  for (int row = 0; row < rect.h; ++row)
    memcpy(destination + row * rect.w, source + (rect.y + row) * stride + rect.x,
           size_t(rect.w) * sizeof(uint16_t));
}

} // namespace bluetooth_mouse
#endif
