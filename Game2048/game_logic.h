// Pure 2048 move logic (no Arduino dependencies) so it can be unit tested
// on the host as well as compiled into the sketch.
//
// Board layout: ROWS (5) rows x COLS (4) columns.
// Direction: 0 = left, 1 = right, 2 = up, 3 = down.

#ifndef GAME2048_LOGIC_H
#define GAME2048_LOGIC_H

#include <stdint.h>
#include <string.h>

namespace game2048
{

static const uint8_t ROWS = 5;
static const uint8_t COLS = 4;

struct TileMove
{
  uint8_t fromR, fromC;
  uint8_t toR, toC;
  uint32_t value;
};

struct MoveResult
{
  bool changed;
  uint32_t gained;
  uint8_t moveCount;
  TileMove moves[ROWS * COLS];
};

inline void addMove(MoveResult &res, uint8_t fromR, uint8_t fromC,
                    uint8_t toR, uint8_t toC, uint32_t value)
{
  TileMove &m = res.moves[res.moveCount++];
  m.fromR = fromR;
  m.fromC = fromC;
  m.toR = toR;
  m.toC = toC;
  m.value = value;
}

// Applies a move to `board`, writing the resulting board to `out` and
// recording every tile's start/end cell in `res` for animation.
inline void move(const uint32_t board[ROWS][COLS], uint8_t dir,
                 uint32_t out[ROWS][COLS], MoveResult &res)
{
  memset(out, 0, sizeof(uint32_t) * ROWS * COLS);
  res.changed = false;
  res.gained = 0;
  res.moveCount = 0;

  const bool horizontal = (dir == 0 || dir == 1);
  const uint8_t lineCount = horizontal ? ROWS : COLS;
  const uint8_t lineLen = horizontal ? COLS : ROWS;

  for (uint8_t line = 0; line < lineCount; line++)
  {
    struct Item
    {
      uint8_t r, c;
      uint32_t v;
    };
    Item items[ROWS]; // lineLen <= max(ROWS, COLS) == ROWS
    uint8_t n = 0;

    // Collect the non-empty tiles, front-most first.
    for (uint8_t k = 0; k < lineLen; k++)
    {
      uint8_t r, c;
      switch (dir)
      {
      case 0: r = line; c = k; break;                 // from left edge
      case 1: r = line; c = COLS - 1 - k; break;      // from right edge
      case 2: r = k; c = line; break;                 // from top edge
      default: r = ROWS - 1 - k; c = line; break;     // from bottom edge
      }
      if (board[r][c] != 0)
      {
        items[n].r = r;
        items[n].c = c;
        items[n].v = board[r][c];
        n++;
      }
    }

    uint8_t target = 0;
    for (uint8_t i = 0; i < n;)
    {
      uint8_t tr, tc;
      switch (dir)
      {
      case 0: tr = line; tc = target; break;
      case 1: tr = line; tc = COLS - 1 - target; break;
      case 2: tr = target; tc = line; break;
      default: tr = ROWS - 1 - target; tc = line; break;
      }

      if (i + 1 < n && items[i].v == items[i + 1].v)
      {
        const uint32_t merged = items[i].v * 2;
        out[tr][tc] = merged;
        res.gained += merged;
        res.changed = true;
        addMove(res, items[i].r, items[i].c, tr, tc, items[i].v);
        addMove(res, items[i + 1].r, items[i + 1].c, tr, tc, items[i + 1].v);
        i += 2;
      }
      else
      {
        out[tr][tc] = items[i].v;
        if (items[i].r != tr || items[i].c != tc)
        {
          res.changed = true;
        }
        addMove(res, items[i].r, items[i].c, tr, tc, items[i].v);
        i++;
      }
      target++;
    }
  }
}

inline bool hasMoves(const uint32_t board[ROWS][COLS])
{
  for (uint8_t r = 0; r < ROWS; r++)
  {
    for (uint8_t c = 0; c < COLS; c++)
    {
      if (board[r][c] == 0)
      {
        return true;
      }
      if (c + 1 < COLS && board[r][c] == board[r][c + 1])
      {
        return true;
      }
      if (r + 1 < ROWS && board[r][c] == board[r + 1][c])
      {
        return true;
      }
    }
  }
  return false;
}

inline uint32_t maxTile(const uint32_t board[ROWS][COLS])
{
  uint32_t m = 0;
  for (uint8_t r = 0; r < ROWS; r++)
  {
    for (uint8_t c = 0; c < COLS; c++)
    {
      if (board[r][c] > m)
      {
        m = board[r][c];
      }
    }
  }
  return m;
}

} // namespace game2048

#endif
