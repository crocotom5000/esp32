#include "../weather_model.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

using namespace dashboard;

int main()
{
  assert(validLocation(51.5072, -0.1276));
  assert(validLocation(-90, 180));
  assert(!validLocation(90.01, 0));
  assert(!validLocation(0, -180.01));
  assert(!validLocation(NAN, 0));
  assert(!validLocation(0, INFINITY));

  Weather weather;
  weather.temperature = -8.5;
  weather.feelsLike = -14;
  weather.humidity = 80;
  weather.wind = 25;
  assert(validWeather(weather));
  weather.humidity = 101;
  assert(!validWeather(weather));
  weather.humidity = 80;
  weather.wind = -1;
  assert(!validWeather(weather));
  weather.wind = 25;
  weather.temperature = NAN;
  assert(!validWeather(weather));

  const int codes[] = {0, 1, 2, 3, 45, 48, 51, 53, 55, 56, 57, 61, 63, 65,
                       66, 67, 71, 73, 75, 77, 80, 81, 82, 85, 86, 95, 96, 99};
  for (int code : codes)
  {
    assert(strcmp(weatherLabel(code, true), "Unknown") != 0);
    assert(strlen(weatherLabel(code, true)) <= 19); // Fits the weather card at 2x.
    assert(weatherIcon(code, true) != Icon::UNKNOWN);
  }
  assert(strcmp(weatherLabel(0, false), "Clear night") == 0);
  assert(weatherIcon(0, false) == Icon::MOON);
  assert(weatherIcon(0, true) == Icon::SUN);
  assert(weatherIcon(48, true) == Icon::FOG);
  assert(weatherIcon(67, true) == Icon::RAIN);
  assert(weatherIcon(86, true) == Icon::SNOW);
  assert(weatherIcon(99, true) == Icon::STORM);
  assert(weatherIcon(42, true) == Icon::UNKNOWN);

  assert(!elapsed(9999, 0, 10000));
  assert(elapsed(10000, 0, 10000));
  assert(!elapsed(3, UINT32_MAX - 5, 10));
  assert(elapsed(4, UINT32_MAX - 5, 10));
  assert(signalBars(-40) == 4);
  assert(signalBars(-55) == 4);
  assert(signalBars(-56) == 3);
  assert(signalBars(-65) == 3);
  assert(signalBars(-75) == 2);
  assert(signalBars(-90) == 1);
  puts("Weather model tests passed");
}
