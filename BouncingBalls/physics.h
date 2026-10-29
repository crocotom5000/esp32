#pragma once

#include <math.h>
#include <stddef.h>

namespace bouncingballs
{

struct Ball
{
  float x, y;
  float vx, vy; // pixels per second; all balls have equal mass
  float radius;
};

struct Bounds
{
  float left, top, right, bottom;
};

inline void bounceWalls(Ball &ball, const Bounds &bounds)
{
  if (ball.x < bounds.left + ball.radius)
  {
    ball.x = bounds.left + ball.radius;
    ball.vx = fabsf(ball.vx);
  }
  else if (ball.x > bounds.right - ball.radius)
  {
    ball.x = bounds.right - ball.radius;
    ball.vx = -fabsf(ball.vx);
  }
  if (ball.y < bounds.top + ball.radius)
  {
    ball.y = bounds.top + ball.radius;
    ball.vy = fabsf(ball.vy);
  }
  else if (ball.y > bounds.bottom - ball.radius)
  {
    ball.y = bounds.bottom - ball.radius;
    ball.vy = -fabsf(ball.vy);
  }
}

inline void collide(Ball &a, Ball &b)
{
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float distanceSquared = dx * dx + dy * dy;
  const float diameter = a.radius + b.radius;
  if (distanceSquared > diameter * diameter) return;

  const float distance = sqrtf(distanceSquared);
  float nx = 1.0f;
  float ny = 0.0f;
  if (distance > 0.00001f)
  {
    nx = dx / distance;
    ny = dy / distance;
  }
  else
  {
    // Coincident centers have no geometric normal. Separate along the incoming
    // relative velocity instead, without dividing by zero.
    const float rvx = a.vx - b.vx;
    const float rvy = a.vy - b.vy;
    const float speed = sqrtf(rvx * rvx + rvy * rvy);
    if (speed > 0.00001f)
    {
      nx = rvx / speed;
      ny = rvy / speed;
    }
  }

  // Remove overlap even if the balls are already moving apart.
  const float correction = (diameter - distance) * 0.5f;
  a.x -= nx * correction;
  a.y -= ny * correction;
  b.x += nx * correction;
  b.y += ny * correction;

  const float closingSpeed = (a.vx - b.vx) * nx + (a.vy - b.vy) * ny;
  if (closingSpeed <= 0.0f) return;

  // Elastic equal-mass collision: exchange normal velocity components while
  // leaving tangential components unchanged. No friction or energy loss.
  a.vx -= closingSpeed * nx;
  a.vy -= closingSpeed * ny;
  b.vx += closingSpeed * nx;
  b.vy += closingSpeed * ny;
}

inline void step(Ball *balls, size_t count, const Bounds &bounds, float dt)
{
  for (size_t i = 0; i < count; i++)
  {
    balls[i].x += balls[i].vx * dt;
    balls[i].y += balls[i].vy * dt;
    bounceWalls(balls[i], bounds);
  }

  // Revisit contacts so a ball pushed into a neighbor or wall gets corrected.
  for (unsigned pass = 0; pass < 4; pass++)
  {
    for (size_t i = 0; i < count; i++)
    {
      for (size_t j = i + 1; j < count; j++) collide(balls[i], balls[j]);
    }
    for (size_t i = 0; i < count; i++) bounceWalls(balls[i], bounds);
  }
}

} // namespace bouncingballs
