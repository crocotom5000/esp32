#ifndef WINDOW_MANAGER_MODEL_H
#define WINDOW_MANAGER_MODEL_H

#include <stdint.h>
#include "render_regions.h"

namespace window_manager {

enum {
  WIDTH = 280, HEIGHT = 456, DESKTOP_TOP = 72, DESKTOP_BOTTOM = 408,
  WINDOW_WIDTH = 176, WINDOW_HEIGHT = 164, TITLE_HEIGHT = 26, SHADOW = 4,
  MAX_WINDOWS = 12,
  VISIBLE_TITLE_WIDTH = 48,
  MIN_WINDOW_X = VISIBLE_TITLE_WIDTH - WINDOW_WIDTH,
  MAX_WINDOW_X = WIDTH - VISIBLE_TITLE_WIDTH,
  MIN_WINDOW_Y = 0,
  MAX_WINDOW_Y = DESKTOP_BOTTOM - TITLE_HEIGHT
};

inline int bounded(int64_t value, int low, int high) {
  return value < low ? low : value > high ? high : int(value);
}

inline Rect addButton() { return Rect(122, 418, 36, 30); }
inline Rect pairButton() { return Rect(212, 418, 56, 30); }

struct Cursor {
  int x = 140, y = 228;
  uint8_t buttons = 0;
};

struct Window {
  uint16_t id = 0;
  int x = 0, y = 0;
  uint32_t textSeed = 0;
  Rect bounds() const { return Rect(x, y, WINDOW_WIDTH, WINDOW_HEIGHT); }
  Rect paintBounds() const { return Rect(x, y, WINDOW_WIDTH + SHADOW, WINDOW_HEIGHT + SHADOW); }
  Rect titleBar() const { return Rect(x, y, WINDOW_WIDTH, TITLE_HEIGHT); }
  Rect closeButton() const { return Rect(x + WINDOW_WIDTH - 26, y + 3, 22, 20); }
};

class Desktop {
public:
  Window windows[MAX_WINDOWS]; // Back to front; the last entry is active.
  unsigned count = 0;
  Cursor cursor;
  uint32_t revision = 0;

  explicit Desktop(uint32_t seed = 1) : randomState(seed ? seed : 1) { createWindow(); }

  bool createWindow() {
    if (count == MAX_WINDOWS) return false;
    if (count) dirty.add(windows[count - 1].paintBounds());
    Window window;
    window.id = allocateID();
    window.x = 8 + (16 + int(count) * 18) % 88;
    window.y = DESKTOP_TOP + 32 + int(count * 22 % 120);
    window.textSeed = randomWord();
    windows[count++] = window;
    dirty.add(window.paintBounds());
    dirty.add(Rect(0, DESKTOP_BOTTOM, WIDTH, HEIGHT - DESKTOP_BOTTOM));
    ++revision;
    return true;
  }

  bool closeWindow(unsigned index) {
    if (index >= count) return false;
    const bool wasActive = index + 1 == count;
    dirty.add(windows[index].paintBounds()); // Restore the removed window and its shadow.
    for (unsigned i = index; i + 1 < count; ++i) windows[i] = windows[i + 1];
    --count;
    if (wasActive && count) dirty.add(windows[count - 1].paintBounds());
    dirty.add(Rect(0, DESKTOP_BOTTOM, WIDTH, HEIGHT - DESKTOP_BOTTOM));
    if (dragging == int(index)) dragging = -1;
    else if (dragging > int(index)) --dragging;
    ++revision;
    return true;
  }

  uint16_t activeWindowID() const { return count ? windows[count - 1].id : 0; }

  int hitTest(int x, int y) const {
    // Windows may extend behind the dock, but its opaque surface owns clicks.
    if (x < 0 || x >= WIDTH || y < 0 || y >= DESKTOP_BOTTOM) return -1;
    for (int i = int(count) - 1; i >= 0; --i)
      if (windows[i].bounds().contains(x, y)) return i;
    return -1;
  }

  // Returns true only for an explicit fresh press on the Pair dock button.
  bool input(int32_t dx, int32_t dy, uint8_t buttons) {
    const Cursor previous = cursor;
    cursor.x = bounded(int64_t(cursor.x) + dx, 0, WIDTH - 12);
    cursor.y = bounded(int64_t(cursor.y) + dy, 0, HEIGHT - 19);
    cursor.buttons = buttons;
    if (cursor.x != previous.x || cursor.y != previous.y) {
      dirty.add(cursorRect(previous.x, previous.y));
      dirty.add(cursorRect(cursor.x, cursor.y));
    }
    if (addButton().contains(previous.x, previous.y) != addButton().contains(cursor.x, cursor.y) ||
        pairButton().contains(previous.x, previous.y) != pairButton().contains(cursor.x, cursor.y)) {
      dirty.add(Rect(0, DESKTOP_BOTTOM, WIDTH, HEIGHT - DESKTOP_BOTTOM));
      ++revision;
    }
    const bool left = buttons & 1, wasLeft = previous.buttons & 1;
    if (suppressUntilRelease) {
      if (!left) suppressUntilRelease = false;
      return false;
    }
    if (left && !wasLeft) {
      if (pairButton().contains(cursor.x, cursor.y)) { cancelInput(); return true; }
      if (addButton().contains(cursor.x, cursor.y)) createWindow();
      else {
        const int hit = hitTest(cursor.x, cursor.y);
        if (hit >= 0) {
          // Check only the topmost hit, and before title-bar drag/focus handling.
          if (windows[hit].closeButton().contains(cursor.x, cursor.y)) {
            closeWindow(unsigned(hit));
            return false;
          }
          raise(unsigned(hit));
          Window &window = windows[count - 1];
          if (window.titleBar().contains(cursor.x, cursor.y)) {
            dragging = int(count) - 1;
            grabX = cursor.x - window.x;
            grabY = cursor.y - window.y;
          }
        }
      }
    }
    if (dragging >= 0 && (left || wasLeft)) {
      Window &window = windows[dragging];
      const int x = bounded(cursor.x - grabX, MIN_WINDOW_X, MAX_WINDOW_X);
      // Let the body spill off-screen, but never lose the title bar above the
      // display or underneath the dock. Keep a grabbable strip at either side.
      const int y = bounded(cursor.y - grabY, MIN_WINDOW_Y, MAX_WINDOW_Y);
      if (x != window.x || y != window.y) {
        dirty.add(window.paintBounds());
        window.x = x;
        window.y = y;
        dirty.add(window.paintBounds());
        ++revision;
      }
    }
    if (!left) dragging = -1;
    return false;
  }

  void cancelInput() {
    dragging = -1;
    cursor.buttons = 0;
    suppressUntilRelease = true; // Lost/reconnected input cannot create a phantom click.
  }

  bool isDragging() const { return dragging >= 0; }

  Damage takeDamage() {
    const Damage result = dirty;
    dirty = Damage();
    return result;
  }

private:
  Damage dirty;
  int dragging = -1, grabX = 0, grabY = 0;
  bool suppressUntilRelease = false;
  uint16_t nextID = 1;
  uint32_t randomState;

  uint16_t allocateID() {
    // Closing/reopening can cycle far beyond MAX_WINDOWS. Skip zero and IDs
    // still in use when the counter wraps, preserving valid titles/colors.
    while (true) {
      const uint16_t id = nextID++;
      if (!nextID) nextID = 1;
      bool used = false;
      for (unsigned i = 0; i < count; ++i) used |= windows[i].id == id;
      if (!used) return id;
    }
  }

  uint32_t randomWord() {
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return randomState;
  }

  void raise(unsigned index) {
    if (index == count - 1) return;
    const Window selected = windows[index];
    dirty.add(selected.paintBounds());
    dirty.add(windows[count - 1].paintBounds());
    for (unsigned i = index; i + 1 < count; ++i) windows[i] = windows[i + 1];
    windows[count - 1] = selected;
    ++revision;
  }
};

} // namespace window_manager
#endif
