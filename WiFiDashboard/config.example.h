#pragma once

#include <stdint.h>

// Copy this file to config.h and edit the copy. config.h is ignored by Git.
// Use a 2.4 GHz network. An empty password is allowed for an open network.
static const char WIFI_SSID[] = "";
static const char WIFI_PASSWORD[] = "";

// Example location only: London. Set all three for your own location.
static const char LOCATION_NAME[] = "London";
static const double LATITUDE = 51.5072;
static const double LONGITUDE = -0.1276;

// POSIX TZ rule, NOT an IANA name such as Europe/London. Includes DST.
// London: GMT0BST,M3.5.0/1,M10.5.0
// New York: EST5EDT,M3.2.0,M11.1.0
// UTC: UTC0
static const char TIMEZONE[] = "EST5EDT,M3.2.0,M11.1.0"
static const uint8_t DISPLAY_BRIGHTNESS = 180; // 0..255
