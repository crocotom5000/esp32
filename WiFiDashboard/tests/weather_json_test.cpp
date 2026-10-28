#include "../weather_json.h"
#include <assert.h>
#include <stdio.h>
#include <string>

using namespace dashboard;

static const char *VALID = R"({"current":{"temperature_2m":13.2,"apparent_temperature":12.3,"relative_humidity_2m":81,"wind_speed_10m":6.8,"weather_code":3,"is_day":0}})";

void rejectsReplacement(const char *before, const char *after)
{
  std::string json = VALID;
  const size_t at = json.find(before);
  assert(at != std::string::npos);
  json.replace(at, std::string(before).size(), after);
  Weather weather;
  weather.temperature = 42;
  assert(!parseWeather(json.c_str(), weather));
  assert(weather.temperature == 42); // Failed parsing must not replace cached data.
}

int main(int argc, char **argv)
{
  Weather weather;
  assert(parseWeather(VALID, weather));
  assert(weather.temperature == 13.2);
  assert(weather.feelsLike == 12.3);
  assert(weather.humidity == 81);
  assert(weather.wind == 6.8);
  assert(weather.code == 3);
  assert(!weather.isDay);
  assert(!parseWeather("{", weather));
  assert(!parseWeather("{}", weather));
  assert(!parseWeather("null", weather));
  assert(!parseWeather("{\"current\":null}", weather));
  assert(!parseWeather("{\"error\":true,\"reason\":\"bad request\"}", weather));
  assert(!parseWeather((std::string(VALID) + "garbage").c_str(), weather));
  rejectsReplacement("13.2", "null");
  rejectsReplacement("13.2", "\"13.2\"");
  rejectsReplacement("13.2", "1e999");
  rejectsReplacement("13.2", "71");
  rejectsReplacement("12.3", "-151");
  rejectsReplacement("81,", "101,");
  rejectsReplacement("6.8", "-1");
  rejectsReplacement("\"weather_code\":3", "\"weather_code\":3.5");
  rejectsReplacement("\"weather_code\":3", "\"weather_code\":100");
  rejectsReplacement("\"is_day\":0", "\"is_day\":2");
  rejectsReplacement("\"is_day\":0", "\"is_day\":false");
  rejectsReplacement("\"wind_speed_10m\":6.8,", "");

  if (argc > 1)
  {
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    char buffer[4097] = {};
    const size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    assert(length > 0 && !ferror(file) && feof(file));
    fclose(file);
    assert(parseWeather(buffer, weather));
  }
  puts("Weather JSON tests passed");
}
