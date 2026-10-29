#include "../motion.h"
#include <assert.h>
#include <stdio.h>

using namespace bouncingballs;

int main()
{
  MotionFilter filter;
  // Startup in any orientation must not cause a kick; ignore ordinary noise.
  assert(!hasMotion(filter.update(600, -300, 740, 0.01f)));
  assert(!hasMotion(filter.update(605, -307, 750, 0.01f)));
  assert(!hasMotion(filter.update(NAN, 0, 0, 0.01f)));
  assert(!hasMotion(filter.update(0, 0, 0, 0)));

  filter = MotionFilter();
  filter.update(0, 0, 1000, 0.01f);
  MotionForce force = filter.update(0, 1000, 1000, 0.01f);
  assert(force.x > 900 && force.y == 0 && force.scatter == 0);
  // A held orientation settles back to zero rather than adding permanent gravity.
  for (unsigned i = 0; i < 300; i++) force = filter.update(0, 1000, 1000, 0.01f);
  assert(!hasMotion(force));

  filter = MotionFilter();
  filter.update(0, 0, 1000, 0.01f);
  force = filter.update(-1000, 0, 1000, 0.01f);
  assert(force.x == 0 && force.y < -900);
  filter = MotionFilter();
  filter.update(0, 0, 1000, 0.01f);
  force = filter.update(0, 0, 2500, 0.01f);
  assert(force.scatter > 1500 && force.x == 0 && force.y == 0);

  Ball balls[] = {{50, 50, 0, 0, 16}, {150, 50, 0, 0, 16}};
  applyMotion(balls, 2, force, 0.1f);
  assert(balls[0].vx > 0 && balls[1].vx < 0);
  assert(fabsf(balls[0].vy) < 0.001f && fabsf(balls[1].vy) < 0.001f);
  applyMotion(balls, 2, {100000, -100000, 100000}, 1.0f);
  for (const Ball &ball : balls)
  {
    assert(isfinite(ball.vx) && isfinite(ball.vy));
    assert(hypotf(ball.vx, ball.vy) <= 320.001f);
  }
  const float vx = balls[0].vx, vy = balls[0].vy;
  applyMotion(balls, 2, {0, 0, 0}, 1.0f);
  assert(balls[0].vx == vx && balls[0].vy == vy);
  puts("Bouncing balls motion tests passed");
}
