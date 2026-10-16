#pragma once

// Copy to clock_config.h and fill in your network locally. The private copy
// is ignored by Git. An empty SSID builds successfully and leaves coils idle.
namespace clock_config {
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";  // Empty is also valid for an open network.

// U.S. Central: CST in winter, CDT in summer, automatic daylight saving.
constexpr char TIME_ZONE[] = "CST6CDT,M3.2.0,M11.1.0";
}  // namespace clock_config
