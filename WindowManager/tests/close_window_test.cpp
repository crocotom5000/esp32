#include "../window_manager.h"
#include <assert.h>
#include <stdio.h>

using namespace window_manager;

static void moveTo(Desktop &desktop, int x, int y, uint8_t buttons = 0) {
  desktop.input(x - desktop.cursor.x, y - desktop.cursor.y, buttons);
}
static void clickClose(Desktop &desktop, const Window &window) {
  const Rect close = window.closeButton();
  moveTo(desktop, close.x + close.w / 2, close.y + close.h / 2);
  desktop.input(0, 0, 1);
  desktop.input(0, 0, 0);
}
static bool covers(const Damage &damage, int x, int y) {
  for (unsigned i = 0; i < damage.count; ++i) if (damage.regions[i].contains(x, y)) return true;
  return false;
}

int main() {
  Desktop desktop;
  const Window initial = desktop.windows[0];
  const Rect close = initial.closeButton();
  assert(initial.titleBar().contains(close.x, close.y));
  assert(initial.titleBar().contains(close.right() - 1, close.bottom() - 1));
  desktop.takeDamage();
  moveTo(desktop, close.x + 11, close.y + 10);
  desktop.input(0, 0, 1);
  assert(desktop.count == 0 && !desktop.isDragging() && desktop.activeWindowID() == 0);
  const Damage removed = desktop.takeDamage();
  const Rect old = initial.paintBounds();
  for (int y = old.y; y < old.bottom(); ++y)
    for (int x = old.x; x < old.right(); ++x) assert(covers(removed, x, y));
  assert(covers(removed, 140, 430)); // Count and enabled + button update too.
  for (int i = 0; i < 10; ++i) desktop.input(0, 0, 1);
  assert(desktop.count == 0); // Holding cannot repeatedly close or drag.
  desktop.input(0, 0, 0);
  assert(desktop.hitTest(close.x, close.y) == -1);
  assert(!desktop.closeWindow(0) && !desktop.closeWindow(MAX_WINDOWS));
  moveTo(desktop, 140, 430);
  desktop.input(0, 0, 1);
  desktop.input(0, 0, 0);
  assert(desktop.count == 1 && desktop.activeWindowID() == 2); // Reopen from empty desktop.

  desktop = Desktop();
  desktop.createWindow();
  const Window underneath = desktop.windows[0];
  const Window foreground = desktop.windows[1];
  desktop.takeDamage();
  clickClose(desktop, foreground);
  assert(desktop.count == 1 && desktop.activeWindowID() == underneath.id);
  const Damage activeChanged = desktop.takeDamage();
  assert(covers(activeChanged, underneath.x, underneath.y)); // Newly active border/title repaint.

  desktop = Desktop();
  desktop.createWindow();
  // The back window's close button is exposed above the front window.
  const Window back = desktop.windows[0];
  clickClose(desktop, back);
  assert(desktop.count == 1 && desktop.activeWindowID() == 2);

  desktop = Desktop();
  desktop.createWindow();
  // Cover the lower window's close button with the upper window's body.
  const Rect hidden = desktop.windows[0].closeButton();
  desktop.windows[1].x = 90;
  desktop.windows[1].y = DESKTOP_TOP;
  moveTo(desktop, hidden.x + 11, hidden.y + 10);
  desktop.input(0, 0, 1);
  assert(desktop.count == 2 && desktop.activeWindowID() == 2 && !desktop.isDragging());
  desktop.input(0, 0, 0);

  // Closing a front window with a coincident close button must not click through.
  desktop.windows[1].x = desktop.windows[0].x;
  desktop.windows[1].y = desktop.windows[0].y;
  const Window top = desktop.windows[1];
  moveTo(desktop, top.closeButton().x + 11, top.closeButton().y + 10);
  desktop.input(0, 0, 1);
  assert(desktop.count == 1 && desktop.activeWindowID() == 1 && !desktop.isDragging());
  desktop.input(0, 0, 1);
  assert(desktop.count == 1);
  desktop.input(0, 0, 0);
  desktop.input(0, 0, 1);
  assert(desktop.count == 0); // A separate fresh click can close the next one.
  desktop.input(0, 0, 0);

  desktop = Desktop();
  const Rect button = desktop.windows[0].closeButton();
  moveTo(desktop, button.x + 11, button.y + 10, 2);
  desktop.input(0, 0, 4);
  desktop.input(0, 0, 0);
  assert(desktop.count == 1); // Only left-click closes.
  moveTo(desktop, button.x - 1, button.y + 10);
  desktop.input(0, 0, 1);
  assert(desktop.count == 1 && desktop.isDragging()); // Adjacent title bar still drags.
  desktop.input(200, 0, 1); // Dragging beyond the edge must not activate close.
  desktop.input(0, 0, 0);
  assert(desktop.count == 1 && !desktop.isDragging());

  while (desktop.count < MAX_WINDOWS) desktop.createWindow();
  assert(desktop.closeWindow(3));
  assert(desktop.count == MAX_WINDOWS - 1 && desktop.createWindow());
  assert(desktop.count == MAX_WINDOWS);
  while (desktop.count) assert(desktop.closeWindow(desktop.count - 1));
  assert(!desktop.closeWindow(0) && desktop.activeWindowID() == 0);
  assert(desktop.createWindow() && desktop.count == 1);

  // A long-lived window remains valid across repeated close/reopen ID rollover.
  desktop = Desktop();
  for (unsigned i = 0; i < 65536; ++i) {
    assert(desktop.createWindow());
    assert(desktop.windows[0].id == 1 && desktop.windows[1].id && desktop.windows[1].id != 1);
    assert(desktop.closeWindow(1));
    desktop.takeDamage();
  }
  puts("Close window tests passed");
}
