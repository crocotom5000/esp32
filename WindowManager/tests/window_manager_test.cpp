#include "../window_manager.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>

using namespace window_manager;

static bool moveTo(Desktop &desktop, int x, int y, uint8_t buttons = 0) {
  return desktop.input(x - desktop.cursor.x, y - desktop.cursor.y, buttons);
}
static void click(Desktop &desktop, int x, int y) {
  moveTo(desktop, x, y);
  desktop.input(0, 0, 1);
  desktop.input(0, 0, 0);
}
static void check(const Desktop &desktop) {
  assert(desktop.count <= MAX_WINDOWS);
  assert(desktop.cursor.x >= 0 && desktop.cursor.x <= WIDTH - 12);
  assert(desktop.cursor.y >= 0 && desktop.cursor.y <= HEIGHT - 19);
  for (unsigned i = 0; i < desktop.count; ++i) {
    const Window &w = desktop.windows[i];
    assert(w.x >= MIN_WINDOW_X && w.x <= MAX_WINDOW_X);
    assert(w.y >= MIN_WINDOW_Y && w.y <= MAX_WINDOW_Y);
    const Rect title = w.titleBar().clipped(WIDTH, DESKTOP_BOTTOM);
    assert(title.w >= VISIBLE_TITLE_WIDTH && title.h == TITLE_HEIGHT);
    for (unsigned j = i + 1; j < desktop.count; ++j) assert(w.id != desktop.windows[j].id);
  }
}

int main() {
  Desktop desktop(1234);
  assert(desktop.count == 1 && desktop.windows[0].id == 1);
  const Window initial = desktop.windows[0];
  check(desktop);
  moveTo(desktop, initial.x + 30, initial.y + 12);
  desktop.input(0, 0, 1);
  assert(desktop.isDragging() && desktop.windows[0].x == initial.x); // No jump on grab.
  desktop.input(25, 20, 1);
  assert(desktop.windows[0].x == initial.x + 25 && desktop.windows[0].y == initial.y + 20);
  desktop.input(5, -3, 0); // Final release packet can still contain motion.
  assert(!desktop.isDragging());
  assert(desktop.windows[0].x == initial.x + 30 && desktop.windows[0].y == initial.y + 17);
  const Window released = desktop.windows[0];
  desktop.input(10, 10, 0);
  assert(desktop.windows[0].x == released.x && desktop.windows[0].y == released.y);
  assert(desktop.windows[0].textSeed == initial.textSeed);

  // A click in the body focuses a window but never starts a drag.
  click(desktop, released.x + 60, released.y + TITLE_HEIGHT + 30);
  desktop.input(0, 0, 1);
  desktop.input(20, 20, 1);
  assert(!desktop.isDragging() && desktop.windows[0].x == released.x);
  desktop.input(0, 0, 0);

  desktop = Desktop(42);
  desktop.createWindow();
  assert(desktop.count == 2 && desktop.windows[1].id == 2);
  const uint32_t seedOne = desktop.windows[0].textSeed;
  click(desktop, 30, 160); // Visible left strip of window 1's body.
  assert(desktop.windows[1].id == 1 && !desktop.isDragging());
  assert(desktop.windows[1].textSeed == seedOne);
  // Window 2's hidden title bar is underneath window 1's body: do not click through.
  moveTo(desktop, 60, 132);
  desktop.input(0, 0, 1);
  assert(desktop.windows[1].id == 1 && !desktop.isDragging());
  desktop.input(0, 0, 0);
  click(desktop, 210, 170); // Exposed right side of window 2.
  assert(desktop.windows[1].id == 2 && !desktop.isDragging());
  moveTo(desktop, desktop.windows[1].x + 12, desktop.windows[1].y + 12);
  desktop.input(0, 0, 1);
  desktop.input(INT32_MAX, INT32_MAX, 1);
  assert(desktop.isDragging() && desktop.windows[1].id == 2);
  assert(desktop.windows[1].x == MAX_WINDOW_X);
  assert(desktop.windows[1].y == MAX_WINDOW_Y);
  desktop.input(INT32_MIN, INT32_MIN, 1);
  assert(desktop.windows[1].x == -12 && desktop.windows[1].y == MIN_WINDOW_Y);
  desktop.input(0, 0, 0);
  check(desktop);

  // Dock is reachable with the cursor; a held button cannot spawn repeatedly.
  desktop = Desktop();
  moveTo(desktop, 140, 430);
  desktop.input(0, 0, 1);
  assert(desktop.count == 2 && desktop.windows[1].id == 2);
  for (int i = 0; i < 10; ++i) desktop.input(0, 0, 1);
  assert(desktop.count == 2);
  desktop.input(0, 0, 0);
  desktop.input(0, 0, 1);
  desktop.input(0, 0, 0);
  assert(desktop.count == 3 && desktop.windows[2].id == 3);
  const Damage pending = desktop.takeDamage(); // Press/release between frames is preserved.
  assert(pending.count);
  click(desktop, addButton().right(), 430);
  assert(desktop.count == 3); // Exclusive right edge of the button.
  while (desktop.count < MAX_WINDOWS) click(desktop, 140, 430);
  assert(!desktop.createWindow());
  click(desktop, 140, 430);
  assert(desktop.count == MAX_WINDOWS);
  check(desktop);

  // Right/middle buttons and blank desktop clicks do not create or raise windows.
  desktop = Desktop();
  moveTo(desktop, 140, 430, 2);
  desktop.input(0, 0, 4);
  desktop.input(0, 0, 0);
  assert(desktop.count == 1);
  click(desktop, 275, 200);
  assert(desktop.count == 1 && desktop.windows[0].id == 1);
  assert(moveTo(desktop, 240, 430, 1)); // Pair request is explicit, separate from window actions.
  assert(!desktop.isDragging());
  desktop.input(0, 0, 0);

  // Disconnect/overflow cancellation releases capture and suppresses a stale held press.
  moveTo(desktop, desktop.windows[0].x + 10, desktop.windows[0].y + 10);
  desktop.input(0, 0, 1);
  assert(desktop.isDragging());
  desktop.cancelInput();
  assert(!desktop.isDragging());
  moveTo(desktop, 140, 430, 1);
  assert(desktop.count == 1);
  desktop.input(0, 0, 0);
  desktop.input(0, 0, 1);
  assert(desktop.count == 2);
  desktop.input(0, 0, 0);

  // Renderer damage includes old/new window shadows and is safe for the panel.
  desktop = Desktop();
  desktop.takeDamage();
  const Window old = desktop.windows[0];
  moveTo(desktop, old.x + 10, old.y + 10);
  desktop.input(0, 0, 1);
  desktop.input(20, 15, 1);
  const Window moved = desktop.windows[0];
  const Damage damage = desktop.takeDamage();
  for (const Window *w : {&old, &moved}) {
    const Rect r = w->paintBounds();
    for (int y = r.y; y < r.bottom(); ++y) for (int x = r.x; x < r.right(); ++x) {
      bool covered = false;
      for (unsigned i = 0; i < damage.count; ++i) covered |= damage.regions[i].contains(x, y);
      assert(covered);
    }
  }
  for (unsigned i = 0; i < damage.count; ++i) {
    const Rect &r = damage.regions[i];
    assert(!(r.x & 1) && !(r.y & 1) && !(r.w & 1) && !(r.h & 1));
    assert(r.x >= 0 && r.y >= 0 && r.right() <= WIDTH && r.bottom() <= HEIGHT);
  }

  uint32_t random = 123;
  for (int i = 0; i < 20000; ++i) {
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
    desktop.input(int(random % 601) - 300, int((random >> 10) % 601) - 300, uint8_t((random >> 22) & 7));
    if (i % 997 == 0) desktop.cancelInput();
    if (i % 503 == 0) desktop.createWindow();
    desktop.takeDamage();
    check(desktop);
  }
  puts("Window manager tests passed");
}
