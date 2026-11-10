#pragma once
#include <stdint.h>
#include <stdlib.h>

namespace trackpad {

enum class Zone { NONE, PAD, SCROLL, LEFT, RIGHT, DRAG };

inline Zone zoneAt(int x, int y) {
  if (x < 12 || x >= 268) return Zone::NONE;
  if (y >= 112 && y < 354) return x < 238 ? Zone::PAD : Zone::SCROLL;
  if (y >= 370 && y < 420) {
    if (x < 94) return Zone::LEFT;
    if (x < 182) return Zone::RIGHT;
    return Zone::DRAG;
  }
  return Zone::NONE;
}

struct Event {
  int dx = 0, dy = 0, wheel = 0;
  uint8_t click = 0;
  bool dragChanged = false;
};

class Gestures {
 public:
  bool dragging = false;

  void reset() {
    down_ = false;
    dragging = false;
    zone_ = Zone::NONE;
    scroll_ = 0;
  }

  Event update(bool down, int x, int y, uint32_t now) {
    Event event;
    if (down && !down_) {
      zone_ = zoneAt(x, y);
      startX_ = lastX_ = x;
      startY_ = lastY_ = y;
      started_ = now;
      moved_ = false;
      scroll_ = 0;
    } else if (down && down_) {
      if (abs(x - startX_) > 8 || abs(y - startY_) > 8) moved_ = true;
      // Leaving a control cancels it until the finger is lifted.
      if (zoneAt(x, y) != zone_) zone_ = Zone::NONE;
      int dx = x - lastX_, dy = y - lastY_;
      if (abs(dx) <= 60 && abs(dy) <= 60) {
        if (zone_ == Zone::PAD) {
          event.dx = dx * 2;
          event.dy = dy * 2;
        } else if (zone_ == Zone::SCROLL) {
          scroll_ -= dy;
          event.wheel = scroll_ / 18;
          scroll_ %= 18;
        }
      }
      lastX_ = x;
      lastY_ = y;
    } else if (!down && down_) {
      if (!moved_ && uint32_t(now - started_) <= 250) {
        if (zone_ == Zone::PAD || zone_ == Zone::LEFT) event.click = 1;
        if (zone_ == Zone::RIGHT) event.click = 2;
        if (zone_ == Zone::DRAG) {
          dragging = !dragging;
          event.dragChanged = true;
        }
      }
      zone_ = Zone::NONE;
    }
    down_ = down;
    return event;
  }

 private:
  bool down_ = false, moved_ = false;
  Zone zone_ = Zone::NONE;
  int startX_ = 0, startY_ = 0, lastX_ = 0, lastY_ = 0, scroll_ = 0;
  uint32_t started_ = 0;
};

}  // namespace trackpad
