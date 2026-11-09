#include "../window_manager.h"
#include <assert.h>
#include <stdio.h>

using namespace window_manager;

static void moveTo(Desktop &desktop, int x, int y, uint8_t buttons = 0) {
  desktop.input(x - desktop.cursor.x, y - desktop.cursor.y, buttons);
}
static void grab(Desktop &desktop, int offsetX, int offsetY) {
  const Window window = desktop.windows[desktop.count - 1];
  moveTo(desktop, window.x + offsetX, window.y + offsetY);
  desktop.input(0, 0, 1);
  assert(desktop.isDragging());
}
static void reachable(const Window &window) {
  const Rect visible = window.titleBar().clipped(WIDTH, DESKTOP_BOTTOM);
  assert(window.y >= 0);
  assert(visible.w >= VISIBLE_TITLE_WIDTH && visible.h == TITLE_HEIGHT);
}
static bool covers(const Damage &damage, int x, int y) {
  for (unsigned i = 0; i < damage.count; ++i) if (damage.regions[i].contains(x, y)) return true;
  return false;
}

int main() {
  Desktop desktop;
  const Window original = desktop.windows[0];
  desktop.takeDamage();
  grab(desktop, 136, 12); // To the left of close, with room to pull off the left edge.
  desktop.input(INT32_MIN, 0, 1);
  assert(desktop.windows[0].x == MIN_WINDOW_X && desktop.windows[0].x < 0);
  assert(desktop.windows[0].y == original.y);
  reachable(desktop.windows[0]);
  const Rect visible = desktop.windows[0].paintBounds().clipped(WIDTH, HEIGHT);
  const Damage damage = desktop.takeDamage();
  for (int y = visible.y; y < visible.bottom(); ++y)
    for (int x = visible.x; x < visible.right(); ++x) assert(covers(damage, x, y));
  for (unsigned i = 0; i < damage.count; ++i) {
    const Rect &r = damage.regions[i];
    assert(r.x >= 0 && r.y >= 0 && r.right() <= WIDTH && r.bottom() <= HEIGHT);
    assert(!(r.x & 1) && !(r.y & 1) && !(r.w & 1) && !(r.h & 1));
  }
  desktop.input(0, 0, 0);
  // The exposed title strip remains usable to bring the window back.
  moveTo(desktop, 10, original.y + 12);
  desktop.input(0, 0, 1);
  desktop.input(200, 0, 1);
  desktop.input(0, 0, 0);
  assert(desktop.windows[0].x > 0 && !desktop.isDragging());

  desktop = Desktop();
  grab(desktop, 10, 12);
  desktop.input(INT32_MAX, 0, 1);
  assert(desktop.windows[0].x == MAX_WINDOW_X);
  assert(desktop.windows[0].bounds().right() > WIDTH);
  reachable(desktop.windows[0]);
  desktop.input(0, 0, 0);
  moveTo(desktop, desktop.windows[0].x + 10, original.y + 12);
  desktop.input(0, 0, 1);
  desktop.input(-180, 0, 1);
  desktop.input(0, 0, 0);
  assert(desktop.windows[0].bounds().right() < WIDTH);

  desktop = Desktop();
  grab(desktop, 40, 12);
  desktop.input(0, INT32_MAX, 1);
  assert(desktop.windows[0].y == MAX_WINDOW_Y);
  assert(desktop.windows[0].bounds().bottom() > HEIGHT);
  reachable(desktop.windows[0]);
  // Even though the window's body is behind the dock, it cannot steal clicks.
  assert(desktop.windows[0].bounds().contains(140, 430));
  assert(desktop.hitTest(140, 430) == -1);
  assert(desktop.hitTest(15, DESKTOP_BOTTOM) == -1);
  desktop.input(0, 0, 0);
  moveTo(desktop, 140, 430);
  desktop.input(0, 0, 1);
  desktop.input(0, 0, 0);
  assert(desktop.count == 2); // + still works over the off-screen body.

  desktop = Desktop();
  grab(desktop, 40, TITLE_HEIGHT - 2);
  desktop.input(0, INT32_MIN, 1);
  assert(desktop.windows[0].y == 0); // Hard stop: no title pixels above the display.
  assert(desktop.windows[0].titleBar().bottom() == TITLE_HEIGHT);
  reachable(desktop.windows[0]);
  desktop.input(0, 0, 0);
  grab(desktop, 40, 12);
  desktop.input(0, 100, 1);
  desktop.input(0, 0, 0);
  assert(desktop.windows[0].y == 100); // Top-edge window remains draggable.

  // Passing over close during a captured drag is not a fresh click.
  desktop = Desktop();
  grab(desktop, 160, TITLE_HEIGHT - 2); // Below the close button, inside the title bar.
  desktop.input(0, -desktop.cursor.y + 10, 1);
  assert(desktop.windows[0].y == 0);
  assert(desktop.windows[0].closeButton().contains(desktop.cursor.x, desktop.cursor.y));
  desktop.input(0, 0, 0);
  assert(desktop.count == 1 && !desktop.isDragging());

  // A partly off-left window can still be explicitly closed through visible X.
  desktop = Desktop();
  grab(desktop, 136, 12);
  desktop.input(INT32_MIN, 0, 1);
  desktop.input(0, 0, 0);
  const Rect close = desktop.windows[0].closeButton();
  moveTo(desktop, close.x + close.w / 2, close.y + close.h / 2);
  desktop.input(0, 0, 1);
  desktop.input(0, 0, 0);
  assert(desktop.count == 0);
  assert(desktop.hitTest(-1, 100) == -1 && desktop.hitTest(WIDTH, 100) == -1);
  puts("Off-screen window dragging tests passed");
}
