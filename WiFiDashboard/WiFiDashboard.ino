// Wi-Fi dashboard - Waveshare ESP32-S3-Touch-AMOLED-1.64
// CO5300 AMOLED (280x456, QSPI) + FT3168 touch (I2C).
// HTTPS runs in a worker task; only loop() touches the display and UI state.

#define ESP32QSPI_MAX_PIXELS_AT_ONCE 8192
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "weather_json.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

using namespace dashboard;

static const int16_t TFT_WIDTH = 280;
static const int16_t TFT_HEIGHT = 456;
static const uint8_t TOUCH_ADDR = 0x38;
static const uint32_t WEATHER_INTERVAL = 10 * 60 * 1000;
static const uint32_t WEATHER_RETRY = 60 * 1000;
static const uint32_t TAP_INTERVAL = 10 * 1000;
static const uint32_t STALE_AFTER = 30 * 60 * 1000;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(46, 10, 11, 12, 13, 14);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, 21, 0, TFT_WIDTH, TFT_HEIGHT, 20, 0);
Arduino_Canvas *frame = new Arduino_Canvas(TFT_WIDTH, TFT_HEIGHT, nullptr);

static const uint16_t COL_BG = RGB565_BLACK;
static const uint16_t COL_PANEL = RGB565(16, 24, 34);
static const uint16_t COL_WHITE = RGB565(235, 243, 250);
static const uint16_t COL_MUTED = RGB565(135, 156, 177);
static const uint16_t COL_ACCENT = RGB565(83, 217, 203);
static const uint16_t COL_AMBER = RGB565(255, 196, 88);

struct WeatherResult
{
  Weather weather;
  bool ok = false;
  char error[40] = "";
};

QueueHandle_t results = nullptr;
TaskHandle_t worker = nullptr;
Weather weather;
bool hasWeather = false;
bool busy = false;
bool attempted = false;
bool lastAttemptOK = false;
bool connected = false;
bool dirty = true;
uint32_t lastAttempt = 0;
uint32_t lastSuccess = 0;
uint32_t lastReconnect = 0;
uint32_t lastDraw = 0;
char weatherError[40] = "";

bool clockReady()
{
  return time(nullptr) > 1700000000;
}

bool configured()
{
  return WIFI_SSID[0] != '\0';
}

// ---------------------------------------------------------------------------
// Weather: built-in cJSON and CA bundle, no additional libraries or API key.
// ---------------------------------------------------------------------------

// HTTPClient decodes chunked transfer encoding into this bounded sink. Never
// grow a String based on an untrusted/missing Content-Length header.
class WeatherBody : public Stream
{
public:
  char data[4097] = {};
  size_t length = 0;

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t *buffer, size_t size) override
  {
    if (size > sizeof(data) - 1 - length) return 0;
    memcpy(data + length, buffer, size);
    length += size;
    data[length] = '\0';
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};

WeatherResult fetchWeather()
{
  WeatherResult result;
  NetworkClientSecure client;
  client.useBuiltinCACertBundle(); // Validate the server, never setInsecure().
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setReuse(false);

  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(LATITUDE, 4);
  url += "&longitude=";
  url += String(LONGITUDE, 4);
  url += "&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,weather_code,is_day";
  url += "&temperature_unit=celsius&wind_speed_unit=kmh";

  if (!http.begin(client, url))
  {
    snprintf(result.error, sizeof(result.error), "Weather client failed");
    return result;
  }
  const int status = http.GET();
  if (status == HTTP_CODE_OK)
  {
    // This endpoint returns under 2 KB. Refuse unexpectedly large responses.
    const int length = http.getSize();
    if (length > 4096)
    {
      snprintf(result.error, sizeof(result.error), "Unexpected weather response");
    }
    else
    {
      WeatherBody body;
      const int received = http.writeToStream(&body);
      result.ok = received > 0 && (size_t)received == body.length &&
                  parseWeather(body.data, result.weather);
      if (!result.ok) snprintf(result.error, sizeof(result.error), "Invalid weather data");
    }
  }
  else if (status > 0)
  {
    snprintf(result.error, sizeof(result.error), "Weather HTTP %d", status);
  }
  else
  {
    snprintf(result.error, sizeof(result.error), "Weather connection failed");
  }
  http.end();
  return result;
}

void weatherTask(void *)
{
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const WeatherResult result = fetchWeather();
    xQueueOverwrite(results, &result);
  }
}

void requestWeather(bool manual)
{
  if (!worker || busy || !connected || !clockReady() || !validLocation(LATITUDE, LONGITUDE)) return;
  const uint32_t now = millis();
  const uint32_t interval = manual ? TAP_INTERVAL : (lastAttemptOK ? WEATHER_INTERVAL : WEATHER_RETRY);
  if (attempted && !elapsed(now, lastAttempt, interval)) return;
  attempted = true;
  busy = true;
  lastAttempt = now;
  dirty = true;
  xTaskNotifyGive(worker);
}

// ---------------------------------------------------------------------------
// FT3168 touch. Ignore swipes/holds; one refresh per short tap on release.
// ---------------------------------------------------------------------------

bool i2cRead(uint8_t reg, uint8_t *buffer, uint8_t length)
{
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, length) != length) return false;
  for (uint8_t i = 0; i < length; i++) buffer[i] = Wire.read();
  return true;
}

void pollTouch()
{
  static bool touching = false;
  static bool moved = false;
  static uint16_t startX = 0, startY = 0;
  static uint32_t downAt = 0;
  uint8_t count = 0;
  if (!i2cRead(0x02, &count, 1)) return; // I2C errors are not releases.
  if ((count & 0x0f) != 0)
  {
    uint8_t point[4];
    if (!i2cRead(0x03, point, 4)) return;
    const uint16_t x = ((point[0] & 0x0f) << 8) | point[1];
    const uint16_t y = ((point[2] & 0x0f) << 8) | point[3];
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT) return;
    if (!touching)
    {
      touching = true;
      moved = false;
      startX = x;
      startY = y;
      downAt = millis();
    }
    if (abs((int)x - startX) > 20 || abs((int)y - startY) > 20) moved = true;
  }
  else if (touching)
  {
    touching = false;
    if (!moved && !elapsed(millis(), downAt, 800)) requestWeather(true);
  }
}

// ---------------------------------------------------------------------------
// Drawing: compose a complete frame off-screen to avoid clear/redraw flicker.
// ---------------------------------------------------------------------------

void text(int16_t x, int16_t y, const char *value, uint8_t size, uint16_t color)
{
  frame->setTextSize(size);
  frame->setTextColor(color);
  frame->setCursor(x, y);
  frame->print(value);
}

void centered(int16_t y, const char *value, uint8_t size, uint16_t color)
{
  text((TFT_WIDTH - (int16_t)strlen(value) * 6 * size) / 2, y, value, size, color);
}

void fitted(int16_t x, int16_t y, const char *value, size_t maxChars, uint16_t color)
{
  char buffer[40];
  maxChars = min(maxChars, sizeof(buffer) - 1);
  const size_t length = strlen(value);
  const size_t count = min(length, maxChars);
  for (size_t i = 0; i < count; i++)
  {
    // The built-in font is ASCII. Keep long SSIDs and UTF-8 out of other cards.
    buffer[i] = (value[i] >= 32 && value[i] <= 126) ? value[i] : '?';
  }
  buffer[count] = '\0';
  if (length > maxChars && count >= 3) memcpy(buffer + count - 3, "...", 3);
  text(x, y, buffer, 1, color);
}

void drawIcon(Icon icon)
{
  const int16_t x = 228, y = 231;
  if (icon == Icon::SUN)
  {
    frame->fillCircle(x, y, 12, COL_AMBER);
    for (int i = 0; i < 8; i++)
    {
      const float a = i * PI / 4;
      frame->drawLine(x + cosf(a) * 17, y + sinf(a) * 17,
                      x + cosf(a) * 22, y + sinf(a) * 22, COL_AMBER);
    }
    return;
  }
  if (icon == Icon::MOON)
  {
    frame->fillCircle(x, y, 17, COL_WHITE);
    frame->fillCircle(x + 8, y - 6, 15, COL_PANEL);
    return;
  }
  if (icon == Icon::UNKNOWN)
  {
    text(x - 6, y - 8, "?", 2, COL_MUTED);
    return;
  }
  frame->fillCircle(x - 12, y, 10, COL_MUTED);
  frame->fillCircle(x, y - 7, 14, COL_WHITE);
  frame->fillCircle(x + 14, y, 11, COL_WHITE);
  frame->fillRoundRect(x - 20, y, 44, 12, 5, COL_WHITE);
  if (icon == Icon::RAIN || icon == Icon::SNOW)
  {
    for (int i = -12; i <= 12; i += 12)
    {
      if (icon == Icon::SNOW) frame->fillCircle(x + i, y + 21, 2, COL_ACCENT);
      else frame->drawLine(x + i, y + 17, x + i - 3, y + 24, COL_ACCENT);
    }
  }
  else if (icon == Icon::STORM)
  {
    frame->fillTriangle(x + 1, y + 12, x - 8, y + 24, x + 3, y + 22, COL_AMBER);
    frame->fillTriangle(x - 2, y + 21, x + 8, y + 19, x - 4, y + 33, COL_AMBER);
  }
  else if (icon == Icon::FOG)
  {
    frame->drawFastHLine(x - 20, y + 18, 44, COL_MUTED);
    frame->drawFastHLine(x - 16, y + 24, 36, COL_MUTED);
  }
}

const char *footerStatus(uint32_t now)
{
  if (!configured()) return "Configure Wi-Fi in config.h";
  if (!validLocation(LATITUDE, LONGITUDE)) return "Check location in config.h";
  if (!worker) return "Weather worker unavailable";
  if (!connected) return "Offline - retrying Wi-Fi";
  if (!clockReady()) return "Syncing clock via NTP...";
  if (busy) return "Refreshing weather...";
  if (weatherError[0]) return weatherError;
  if (hasWeather && elapsed(now, lastSuccess, STALE_AFTER)) return "Weather is stale - tap to retry";
  return "Tap anywhere to refresh";
}

void render(uint32_t now)
{
  frame->fillScreen(COL_BG);
  text(20, 20, "WIFI / DASHBOARD", 1, COL_ACCENT);
  frame->fillCircle(253, 23, 4, connected ? COL_ACCENT : COL_AMBER);

  char buffer[48];
  if (clockReady())
  {
    const time_t epoch = time(nullptr);
    struct tm local;
    localtime_r(&epoch, &local);
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y", &local);
    centered(54, buffer, 1, COL_MUTED);
    strftime(buffer, sizeof(buffer), "%H:%M", &local);
    centered(80, buffer, 7, COL_WHITE);
    strftime(buffer, sizeof(buffer), "%Z  /  %S", &local);
    centered(145, buffer, 1, COL_MUTED);
  }
  else
  {
    centered(54, "WAITING FOR NETWORK TIME", 1, COL_MUTED);
    centered(80, "--:--", 7, COL_WHITE);
    centered(145, "LOCAL TIME", 1, COL_MUTED);
  }

  frame->fillRoundRect(12, 174, 256, 146, 16, COL_PANEL);
  fitted(24, 188, LOCATION_NAME, 29, COL_ACCENT);
  if (hasWeather)
  {
    snprintf(buffer, sizeof(buffer), "%.0f C", weather.temperature);
    text(24, 214, buffer, 4, COL_WHITE);
    drawIcon(weatherIcon(weather.code, weather.isDay));
    text(24, 263, weatherLabel(weather.code, weather.isDay), 2, COL_WHITE);
    snprintf(buffer, sizeof(buffer), "Feels %.0f C   Humidity %.0f%%", weather.feelsLike, weather.humidity);
    text(24, 290, buffer, 1, COL_MUTED);
    snprintf(buffer, sizeof(buffer), "Wind %.0f km/h   %lum ago%s", weather.wind,
             (unsigned long)((uint32_t)(now - lastSuccess) / 60000),
             elapsed(now, lastSuccess, STALE_AFTER) ? " !" : "");
    fitted(24, 304, buffer, 38, COL_MUTED);
  }
  else if (!configured())
  {
    text(24, 218, "SETUP REQUIRED", 2, COL_WHITE);
    text(24, 252, "Copy config.example.h to config.h", 1, COL_MUTED);
    text(24, 272, "Set Wi-Fi, location and timezone.", 1, COL_MUTED);
    text(24, 292, "Then rebuild and upload.", 1, COL_MUTED);
  }
  else
  {
    text(24, 218, "WEATHER", 3, COL_WHITE);
    text(24, 263, "Waiting for first update", 1, COL_MUTED);
    text(24, 290, "Celsius / km/h", 1, COL_MUTED);
  }

  frame->fillRoundRect(12, 330, 256, 77, 16, COL_PANEL);
  text(24, 342, connected ? "WI-FI CONNECTED" : "WI-FI DISCONNECTED", 1,
       connected ? COL_ACCENT : COL_AMBER);
  fitted(24, 361, configured() ? WIFI_SSID : "No network configured", 36, COL_WHITE);
  if (connected)
  {
    const int rssi = WiFi.RSSI();
    snprintf(buffer, sizeof(buffer), "%s  %d dBm", WiFi.localIP().toString().c_str(), rssi);
    text(24, 383, buffer, 1, COL_MUTED);
    const int bars = signalBars(rssi);
    for (int i = 0; i < 4; i++)
    {
      const int height = 4 + i * 4;
      frame->fillRect(225 + i * 7, 353 - height, 4, height, i < bars ? COL_ACCENT : COL_MUTED);
    }
  }
  else
  {
    text(24, 383, configured() ? "Use a 2.4 GHz Wi-Fi network" : "Edit config.h to get started", 1, COL_MUTED);
  }

  centered(421, footerStatus(now), 1, COL_MUTED);
  centered(442, "Weather by Open-Meteo.com", 1, COL_MUTED);
  gfx->draw16bitRGBBitmap(0, 0, frame->getFramebuffer(), TFT_WIDTH, TFT_HEIGHT);
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup()
{
  if (!gfx->begin())
  {
    while (true) delay(1000);
  }
  gfx->setBrightness(DISPLAY_BRIGHTNESS);
  gfx->fillScreen(COL_BG);
  // Keep the large canvas out of the internal heap needed by Wi-Fi and TLS.
  // With PSRAM enabled the core places large malloc/aligned_alloc blocks there.
  if (!psramFound() || !frame->begin())
  {
    gfx->setTextColor(COL_WHITE);
    gfx->setCursor(20, 210);
    gfx->print("Enable PSRAM / check framebuffer");
    while (true) delay(1000);
  }
  frame->setTextWrap(false);
  Wire.begin(47, 48, 400000);
  Wire.setTimeOut(20);
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();

  results = xQueueCreate(1, sizeof(WeatherResult));
  if (results)
  {
    if (xTaskCreate(weatherTask, "weather", 16384, nullptr, 1, &worker) != pdPASS) worker = nullptr;
  }
  if (configured())
  {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastReconnect = millis();
  }
  render(millis());
}

void loop()
{
  const uint32_t now = millis();
  const bool online = WiFi.status() == WL_CONNECTED;
  if (online != connected)
  {
    connected = online;
    dirty = true;
    if (connected) configTzTime(TIMEZONE, "pool.ntp.org", "time.cloudflare.com");
  }
  if (configured() && !connected && elapsed(now, lastReconnect, 30000))
  {
    lastReconnect = now;
    WiFi.reconnect();
  }

  WeatherResult result;
  if (results && xQueueReceive(results, &result, 0) == pdTRUE)
  {
    busy = false;
    lastAttemptOK = result.ok;
    snprintf(weatherError, sizeof(weatherError), "%s", result.error);
    if (result.ok)
    {
      weather = result.weather;
      hasWeather = true;
      lastSuccess = now;
    }
    dirty = true;
  }
  pollTouch();
  requestWeather(false);
  if (dirty || elapsed(now, lastDraw, 1000))
  {
    dirty = false;
    lastDraw = now;
    render(now);
  }
  delay(10);
}
