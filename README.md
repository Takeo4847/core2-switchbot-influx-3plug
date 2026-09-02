# M5Stack Core2 + 3 SwitchBot Plug Mini + InfluxDB

This project extends the previous single-plug logger to three Plug Mini devices.

InfluxDB measurements:

- `switchbot_ble`: current power / BLE RSSI / ON-OFF / sequence, each scan cycle
- `switchbot_energy_30m`: energy for each JST hh:00-hh:30 or hh:30-hh:00 window
- `switchbot_energy_day`: cumulative energy from JST 00:00, written every 30-minute boundary

All three devices share the same measurement names and are separated by the `device_id` tag.

## Before build

1. Edit `include/secrets.h`.
2. Replace the MAC addresses for Plug-2 and Plug-3 in `src/main.cpp`.
3. Build/upload with PlatformIO.
4. Open the serial monitor at 115200 baud.

The first known device is pre-filled as `84:FC:E6:B4:9D:E6`.

## Daily accumulator and reboot behavior

The daily accumulator is checkpointed to ESP32 NVS at each completed 30-minute boundary. If the Core2 restarts, completed half-hour contributions from the current day are restored. The unfinished part of the current half-hour is not guessed; after such a restart `coverage_complete=false` shows that the day has a measurement gap.

For a completely gap-free day value, keep the Core2 running across midnight. A future revision can also restore the latest partial accumulator from InfluxDB itself if required.
