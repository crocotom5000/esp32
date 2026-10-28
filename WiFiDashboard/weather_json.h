#pragma once

#include <cJSON.h>
#include "weather_model.h"

namespace dashboard
{

inline bool jsonNumber(const cJSON *object, const char *key, double &value)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return false;
  value = item->valuedouble;
  return true;
}

inline bool parseWeather(const char *json, Weather &output)
{
  // Require the entire body, not just a valid JSON prefix.
  cJSON *root = cJSON_ParseWithOpts(json, nullptr, true);
  if (!root) return false;
  const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
  Weather candidate;
  double code = 0, day = 0;
  bool ok = cJSON_IsObject(current) &&
            jsonNumber(current, "temperature_2m", candidate.temperature) &&
            jsonNumber(current, "apparent_temperature", candidate.feelsLike) &&
            jsonNumber(current, "relative_humidity_2m", candidate.humidity) &&
            jsonNumber(current, "wind_speed_10m", candidate.wind) &&
            jsonNumber(current, "weather_code", code) &&
            jsonNumber(current, "is_day", day) &&
            code >= 0 && code <= 99 && floor(code) == code &&
            (day == 0 || day == 1) && validWeather(candidate);
  if (ok)
  {
    candidate.code = (int)code;
    candidate.isDay = day == 1;
    output = candidate;
  }
  cJSON_Delete(root);
  return ok;
}

} // namespace dashboard
