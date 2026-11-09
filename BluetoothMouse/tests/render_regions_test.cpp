#include "../render_regions.h"
#include <assert.h>
#include <stdio.h>

using namespace bluetooth_mouse;

static bool covers(const Damage &damage, int x, int y) {
  for (unsigned i = 0; i < damage.count; ++i) {
    const Rect &r = damage.regions[i];
    if (x >= r.x && x < r.right() && y >= r.y && y < r.bottom()) return true;
  }
  return false;
}

int main() {
  Damage damage;
  damage.add(Rect());
  damage.add(Rect(-20, -20, 10, 10));
  damage.add(Rect(300, 500, 10, 10));
  assert(damage.count == 0);
  damage.add(cursorRect(140, 228));
  damage.add(cursorRect(144, 230));
  assert(damage.count == 1 && damage.regions[0].area() == 16 * 22);
  assert(covers(damage, 140, 228) && covers(damage, 155, 248));
  assert(!covers(damage, 156, 249));

  damage = Damage();
  damage.add(cursorRect(8, 72));
  damage.add(cursorRect(260, 362));
  assert(damage.count == 2); // A big cursor jump must not become a full-screen blit.
  assert(!covers(damage, 140, 228));
  assert(covers(damage, 271, 380)); // Entire cursor tail, including at canvas edges.
  damage.add(Rect(0, 384, 280, 10));
  assert(damage.count == 3 && covers(damage, 279, 393));

  damage = Damage();
  damage.add(Rect(0, 0, 5, 5));
  damage.add(Rect(10, 0, 5, 5));
  damage.add(Rect(20, 0, 5, 5));
  damage.add(Rect(3, 0, 19, 5)); // Bridges all three regions.
  assert(damage.count == 1 && damage.regions[0].area() == 26 * 6);
  damage.add(Rect(278, 454, 20, 20));
  assert(damage.count == 2 && covers(damage, 279, 455));
  assert(!covers(damage, 280, 456));
  damage.add(Rect(-2, -2, 4, 4));
  assert(damage.count == 2); // Clipped into existing top-left region.

  damage = Damage();
  for (int i = 0; i < 5; ++i) damage.add(Rect(10 * i, 100, 2, 2));
  assert(damage.count <= 4); // Bounded fallback still covers every input.
  for (int i = 0; i < 5; ++i) assert(covers(damage, 10 * i, 100));

  uint16_t source[10 * 8], packed[3 * 4];
  for (unsigned i = 0; i < 80; ++i) source[i] = i;
  packRect(source, 10, Rect(4, 2, 3, 4), packed);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 3; ++x)
      assert(packed[y * 3 + x] == source[(y + 2) * 10 + x + 4]);
  packRect(source, 10, Rect(7, 6, 3, 2), packed);
  assert(packed[0] == 67 && packed[5] == 79);

  // Every cursor position, including odd coordinates, must produce complete
  // two-pixel-aligned transfers that cover the entire cursor and remain on-panel.
  for (int y = 72; y <= 362; ++y) {
    for (int x = 8; x <= 260; ++x) {
      const Rect r = panelRect(cursorRect(x, y));
      assert(!(r.x & 1) && !(r.y & 1) && !(r.w & 1) && !(r.h & 1));
      assert(r.x <= x && r.y <= y && r.right() >= x + 12 && r.bottom() >= y + 19);
      assert(r.x >= 0 && r.y >= 0 && r.right() <= 280 && r.bottom() <= 456);
    }
  }
  const Rect corner = panelRect(Rect(279, 455, 1, 1));
  assert(corner.x == 278 && corner.y == 454 && corner.w == 2 && corner.h == 2);
  for (int width = 2; width <= 280; width += 2) {
    const int rows = stripRows(width, 280 * 16);
    assert(rows >= 2 && !(rows & 1) && width * rows <= 280 * 16);
    // A large smear must not reintroduce odd row starts between staging chunks.
    for (int y = 72; y < 364; y += rows) assert(!(y & 1));
  }
  puts("Render region tests passed");
}
