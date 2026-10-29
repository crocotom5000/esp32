#include "../physics.h"
#include <assert.h>
#include <stdio.h>

using namespace bouncingballs;

static bool near(float a, float b, float tolerance = 0.001f)
{
  return fabsf(a - b) < tolerance;
}

static float energy(const Ball &a, const Ball &b)
{
  return a.vx * a.vx + a.vy * a.vy + b.vx * b.vx + b.vy * b.vy;
}

int main()
{
  const Bounds bounds = {0, 0, 280, 456};
  Ball wall = {10, 10, -100, -80, 10};
  step(&wall, 1, bounds, 1.0f / 240.0f);
  assert(near(wall.x, 10) && near(wall.y, 10));
  assert(near(wall.vx, 100) && near(wall.vy, 80));
  wall = {271, 447, 100, 80, 10};
  bounceWalls(wall, bounds);
  assert(near(wall.x, 270) && near(wall.y, 446));
  assert(near(wall.vx, -100) && near(wall.vy, -80));

  // Head-on impacts exchange normal velocities, not tangential velocities.
  Ball a = {50, 50, 100, 12, 10};
  Ball b = {70, 50, -40, -7, 10};
  collide(a, b);
  assert(near(a.vx, -40) && near(b.vx, 100));
  assert(near(a.vy, 12) && near(b.vy, -7));

  // An oblique impact conserves both momentum and kinetic energy.
  a = {50, 50, 90, 60, 10};
  b = {62, 66, -30, -20, 10};
  const float initialEnergy = energy(a, b);
  collide(a, b);
  assert(near(a.vx + b.vx, 60));
  assert(near(a.vy + b.vy, 40));
  assert(near(energy(a, b), initialEnergy, 0.01f));

  // Separate an overlap without bouncing balls that are already moving apart.
  a = {50, 50, -10, 0, 10};
  b = {65, 50, 10, 0, 10};
  collide(a, b);
  assert(near(b.x - a.x, 20));
  assert(near(a.vx, -10) && near(b.vx, 10));

  a = {50, 50, 0, 0, 10};
  b = a;
  collide(a, b);
  assert(isfinite(a.x) && isfinite(b.x));
  assert(near(b.x - a.x, 20));
  a = {50, 50, 0, 20, 10};
  b = {50, 50, 0, -20, 10};
  collide(a, b);
  assert(near(b.y - a.y, 20));
  assert(near(a.vy, -20) && near(b.vy, 20));

  // Simulate two minutes: finite, inside the arena, and no energy drift.
  Ball balls[] = {{40, 50, 100, 75, 16}, {220, 80, -85, 65, 16},
                  {140, 360, 45, -120, 16}};
  float totalEnergy = 0;
  for (const Ball &ball : balls) totalEnergy += ball.vx * ball.vx + ball.vy * ball.vy;
  for (unsigned tick = 0; tick < 240 * 120; tick++)
  {
    step(balls, 3, bounds, 1.0f / 240.0f);
    for (const Ball &ball : balls)
    {
      assert(isfinite(ball.x) && isfinite(ball.y));
      assert(ball.x >= 16 && ball.x <= 264);
      assert(ball.y >= 16 && ball.y <= 440);
    }
  }
  float finalEnergy = 0;
  for (const Ball &ball : balls) finalEnergy += ball.vx * ball.vx + ball.vy * ball.vy;
  assert(near(finalEnergy, totalEnergy, totalEnergy * 0.0001f));
  puts("Bouncing balls physics tests passed");
}
