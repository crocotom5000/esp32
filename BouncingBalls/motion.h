#pragma once

#include "physics.h"

namespace bouncingballs
{

struct MotionForce
{
  float x, y;    // screen-plane acceleration, pixels/s^2
  float scatter; // out-of-plane shaking, pixels/s^2
};

class MotionFilter
{
public:
  MotionForce update(float ax, float ay, float az, float dt)
  {
    if (!isfinite(ax) || !isfinite(ay) || !isfinite(az) ||
        !isfinite(dt) || dt <= 0.0f) return {0, 0, 0};
    if (!seeded)
    {
      baselineX = ax;
      baselineY = ay;
      baselineZ = az;
      seeded = true;
      return {0, 0, 0};
    }

    // Track steady gravity/orientation, retaining short movements and shakes.
    // Inputs are mg, as configured in the QMI8658 library.
    const float k = 1.0f - expf(-dt / 0.30f);
    baselineX += (ax - baselineX) * k;
    baselineY += (ay - baselineY) * k;
    baselineZ += (az - baselineZ) * k;
    // On this board screen X follows chip Y, and screen Y follows chip X.
    // Signs are opposite the SpiritLevel bubble: balls lag behind a shove.
    return {deadzone((ay - baselineY) / 1000.0f, 0.045f) * 1100.0f,
            deadzone((ax - baselineX) / 1000.0f, 0.045f) * 1100.0f,
            fabsf(deadzone((az - baselineZ) / 1000.0f, 0.18f)) * 1400.0f};
  }

private:
  bool seeded = false;
  float baselineX = 0, baselineY = 0, baselineZ = 0;

  static float deadzone(float value, float threshold)
  {
    return fabsf(value) <= threshold ? 0.0f :
           copysignf(fabsf(value) - threshold, value);
  }
};

inline bool hasMotion(const MotionForce &force)
{
  return force.x != 0.0f || force.y != 0.0f || force.scatter != 0.0f;
}

inline void applyMotion(Ball *balls, size_t count, const MotionForce &force, float dt)
{
  if (!hasMotion(force)) return; // leave ordinary elastic motion untouched
  static const float MAX_SPEED = 320.0f;
  for (size_t i = 0; i < count; i++)
  {
    // Different directions turn a Z-axis shake into a scatter, not a group shove.
    const float angle = 6.2831853f * i / count;
    balls[i].vx += (force.x + cosf(angle) * force.scatter) * dt;
    balls[i].vy += (force.y + sinf(angle) * force.scatter) * dt;
    const float speed = sqrtf(balls[i].vx * balls[i].vx + balls[i].vy * balls[i].vy);
    if (speed > MAX_SPEED)
    {
      balls[i].vx *= MAX_SPEED / speed;
      balls[i].vy *= MAX_SPEED / speed;
    }
  }
}

} // namespace bouncingballs
