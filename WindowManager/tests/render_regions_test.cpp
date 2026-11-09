#include "../render_regions.h"
#include <assert.h>
#include <stdio.h>

using namespace window_manager;

int main() {
  for (int y = 0; y <= 437; ++y) {
    for (int x = 0; x <= 268; ++x) {
      const Rect r = panelRect(cursorRect(x, y));
      assert(!(r.x & 1) && !(r.y & 1) && !(r.w & 1) && !(r.h & 1));
      assert(r.x <= x && r.y <= y && r.right() >= x + 12 && r.bottom() >= y + 19);
      assert(r.x >= 0 && r.y >= 0 && r.right() <= 280 && r.bottom() <= 456);
    }
  }
  Damage damage;
  damage.add(cursorRect(0, 0));
  damage.add(cursorRect(268, 437));
  assert(damage.count == 2); // Do not upload the unchanged area between distant cursors.
  damage.add(Rect(12, 0, 260, 440));
  for (unsigned i = 0; i < damage.count; ++i) {
    const Rect &r = damage.regions[i];
    assert(!(r.x & 1) && !(r.y & 1) && !(r.w & 1) && !(r.h & 1));
  }
  uint16_t source[80], crop[12];
  for (unsigned i = 0; i < 80; ++i) source[i] = i;
  packRect(source, 10, Rect(4, 2, 3, 4), crop);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 3; ++x) assert(crop[y * 3 + x] == source[(y + 2) * 10 + x + 4]);
  for (int width = 2; width <= 280; width += 2) {
    const int rows = stripRows(width, 280 * 16);
    assert(rows >= 2 && !(rows & 1) && rows * width <= 280 * 16);
  }
  puts("Desktop rendering region tests passed");
}
