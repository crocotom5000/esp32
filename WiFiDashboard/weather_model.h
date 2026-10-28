#pragma once

#include <math.h>
#include <stdint.h>

namespace dashboard
{

struct Weather
{
  double temperature = 0;
  double feelsLike = 0;
  double humidity = 0;
  double wind = 0;
  int code = 0;
  bool isDay = true;
};

enum class Icon { SUN, MOON, CLOUD, FOG, RAIN, SNOW, STORM, UNKNOWN };

inline const char *weatherLabel(int code, bool isDay)
{
  switch (code)
  {
  case 0: return isDay ? "Clear sky" : "Clear night";
  case 1: return "Mostly clear";
  case 2: return "Partly cloudy";
  case 3: return "Overcast";
  case 45: case 48: return "Fog";
  case 51: case 53: case 55: return "Drizzle";
  case 56: case 57: return "Icy drizzle";
  case 61: case 63: case 65: return "Rain";
  case 66: case 67: return "Freezing rain";
  case 71: case 73: case 75: case 77: return "Snow";
  case 80: case 81: case 82: return "Rain showers";
  case 85: case 86: return "Snow showers";
  case 95: case 96: case 99: return "Thunderstorm";
  default: return "Unknown";
  }
}

inline Icon weatherIcon(int code, bool isDay)
{
  switch (code)
  {
  case 0: case 1: return isDay ? Icon::SUN : Icon::MOON;
  case 2: case 3: return Icon::CLOUD;
  case 45: case 48: return Icon::FOG;
  case 51: case 53: case 55: case 56: case 57:
  case 61: case 63: case 65: case 66: case 67:
  case 80: case 81: case 82: return Icon::RAIN;
  case 71: case 73: case 75: case 77: case 85: case 86: return Icon::SNOW;
  case 95: case 96: case 99: return Icon::STORM;
  default: return Icon::UNKNOWN;
  }
}

inline bool validLocation(double latitude, double longitude)
{
  return isfinite(latitude) && isfinite(longitude) &&
         latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180;
}

inline bool validWeather(const Weather &weather)
{
  return isfinite(weather.temperature) && weather.temperature >= -100 && weather.temperature <= 70 &&
         isfinite(weather.feelsLike) && weather.feelsLike >= -150 && weather.feelsLike <= 100 &&
         isfinite(weather.humidity) && weather.humidity >= 0 && weather.humidity <= 100 &&
         isfinite(weather.wind) && weather.wind >= 0 && weather.wind <= 500;
}

// Unsigned subtraction keeps short scheduling intervals safe at millis() wrap.
inline bool elapsed(uint32_t now, uint32_t since, uint32_t interval)
{
  return (uint32_t)(now - since) >= interval;
}

inline int signalBars(int rssi)
{
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

} // namespace dashboard
