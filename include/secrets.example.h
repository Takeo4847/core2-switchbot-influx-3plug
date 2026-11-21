#pragma once

// Wi-Fi
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// InfluxDB 2.x write endpoint.
// Keep precision=s because the program sends UNIX timestamps in seconds.
// Example:
// https://your-host.example:8443/api/v2/write?org=YOUR_ORG&bucket=switchbot&precision=s
#define INFLUX_WRITE_URL "https://YOUR_INFLUX_HOST:8443/api/v2/write?org=YOUR_ORG&bucket=switchbot&precision=s"
#define INFLUX_TOKEN     "YOUR_INFLUX_TOKEN"
