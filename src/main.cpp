#include <Arduino.h>
#include <M5Core2.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <time.h>

#include "secrets.h"

// ============================================================
// User configuration
// ============================================================
static const char* GATEWAY_ID = "Core2-1";

struct DeviceConfig {
  const char* mac;
  const char* label;
};

// SwitchBot Plug Mini MAC addresses monitored by this gateway.
static const DeviceConfig DEVICE_CONFIG[] = {
  {"F8:5B:1B:29:48:32", "Plug-1"},
  {"AC:27:6E:40:D8:26", "Plug-2"},
  {"AC:27:6E:41:FD:92", "Plug-3"},
};

static constexpr size_t DEVICE_COUNT = sizeof(DEVICE_CONFIG) / sizeof(DEVICE_CONFIG[0]);
static_assert(DEVICE_COUNT == 3, "This project is configured for three plugs.");

// One BLE scan every cycle. The last advertisement found for each target is used.
static constexpr uint32_t CYCLE_MS = 10000;          // 10 s cycle
static constexpr uint32_t BLE_SCAN_SECONDS = 4;      // 4 s BLE scan
static constexpr uint32_t MAX_VALID_GAP_SEC = 120;   // do not guess power across longer gaps

// SwitchBot company identifier
static constexpr uint16_t SWITCHBOT_COMPANY_ID = 0x0969;

// ============================================================
// Runtime structures
// ============================================================
struct Sample {
  bool valid = false;
  float powerW = 0.0f;
  int rssi = 0;
  bool stateOn = false;
  uint8_t sequence = 0;
  time_t epoch = 0;
};

struct DeviceState {
  String deviceId;                  // MAC without ':'; e.g. 84FCE6B49DE6
  Sample pending;

  bool haveCurrent = false;
  float currentPowerW = 0.0f;
  int currentRssi = 0;
  bool currentStateOn = false;
  uint8_t currentSequence = 0;
  time_t currentEpoch = 0;

  bool haveLastSample = false;
  float lastPowerW = 0.0f;
  time_t lastEpoch = 0;

  // Current 30-minute window
  time_t halfStart = 0;
  double halfWh = 0.0;
  uint32_t halfIntegratedSec = 0;

  // Current local calendar day (JST)
  time_t dayStart = 0;
  double dayWh = 0.0;
  uint32_t dayIntegratedSec = 0;
};

static DeviceState states[DEVICE_COUNT];
static BLEScan* bleScan = nullptr;
static Preferences preferences;
static bool persistRequested = false;

// ============================================================
// Time helpers (JST)
// ============================================================
static time_t getHalfHourStart(time_t t) {
  struct tm localTm;
  localtime_r(&t, &localTm);
  localTm.tm_min = (localTm.tm_min / 30) * 30;
  localTm.tm_sec = 0;
  return mktime(&localTm);
}

static time_t getLocalMidnight(time_t t) {
  struct tm localTm;
  localtime_r(&t, &localTm);
  localTm.tm_hour = 0;
  localTm.tm_min = 0;
  localTm.tm_sec = 0;
  return mktime(&localTm);
}

static String formatLocalTime(time_t t) {
  struct tm localTm;
  localtime_r(&t, &localTm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &localTm);
  return String(buf);
}

// ============================================================
// Device/MAC helpers
// ============================================================
static String macToDeviceId(const char* mac) {
  String s(mac);
  s.replace(":", "");
  s.toUpperCase();
  return s;
}

static String payloadMacToString(const uint8_t* p) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           p[0], p[1], p[2], p[3], p[4], p[5]);
  return String(buf);
}

static int findConfiguredDevice(const String& mac) {
  for (size_t i = 0; i < DEVICE_COUNT; ++i) {
    if (mac.equalsIgnoreCase(DEVICE_CONFIG[i].mac)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// ============================================================
// BLE advertisement parser
// SwitchBot Plug Mini manufacturer data:
// company id(2), MAC(6), seq(1), state(1), flags(1), Wi-Fi RSSI(1), power(2)
// power = 15-bit value / 10 W
// ============================================================
class PlugAdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (!advertisedDevice.haveManufacturerData()) {
      return;
    }

    std::string raw = advertisedDevice.getManufacturerData();
    if (raw.size() < 14) {
      return;
    }

    const uint8_t* b = reinterpret_cast<const uint8_t*>(raw.data());

    // ESP32 BLE normally exposes Company ID little-endian: 69 09 for 0x0969.
    // Accept both byte orders so the parser is tolerant of library differences.
    const bool companyOk =
        ((b[0] == 0x69) && (b[1] == 0x09)) ||
        ((b[0] == 0x09) && (b[1] == 0x69));
    if (!companyOk) {
      return;
    }

    const uint8_t* p = b + 2;
    const String mac = payloadMacToString(p);
    const int index = findConfiguredDevice(mac);
    if (index < 0) {
      return;
    }

    const uint8_t sequence = p[6];
    const bool stateOn = (p[7] & 0x80) != 0;
    const uint16_t rawPower =
        (static_cast<uint16_t>(p[10] & 0x7F) << 8) |
        static_cast<uint16_t>(p[11]);
    const float powerW = static_cast<float>(rawPower) / 10.0f;

    Sample& s = states[index].pending;
    s.valid = true;
    s.powerW = powerW;
    s.rssi = advertisedDevice.getRSSI();
    s.stateOn = stateOn;
    s.sequence = sequence;
    s.epoch = time(nullptr);
  }
};

static PlugAdvertisedCallbacks advCallbacks;

// ============================================================
// Influx line helpers
// ============================================================
static void appendInstantLine(String& batch, size_t i, const Sample& s) {
  char line[320];
  snprintf(line, sizeof(line),
           "switchbot_ble,device_id=%s,gateway=%s "
           "power_w=%.1f,ble_rssi_dbm=%di,state_on=%s,sequence=%ui %ld\n",
           states[i].deviceId.c_str(), GATEWAY_ID,
           s.powerW, s.rssi, s.stateOn ? "true" : "false",
           static_cast<unsigned>(s.sequence), static_cast<long>(s.epoch));
  batch += line;
}

static void appendHalfHourLine(String& batch, size_t i, time_t endEpoch) {
  const DeviceState& d = states[i];
  const bool complete = (d.halfIntegratedSec >= 1800);

  char line[320];
  snprintf(line, sizeof(line),
           "switchbot_energy_30m,device_id=%s,gateway=%s "
           "energy_30min_wh=%.3f,window_seconds=%lui,complete=%s %ld\n",
           d.deviceId.c_str(), GATEWAY_ID,
           d.halfWh, static_cast<unsigned long>(d.halfIntegratedSec),
           complete ? "true" : "false", static_cast<long>(endEpoch));
  batch += line;

  Serial.println();
  Serial.println("=================================");
  Serial.println("30-minute energy window completed");
  Serial.printf("Device: %s\n", d.deviceId.c_str());
  Serial.printf("Energy: %.3f Wh\n", d.halfWh);
  Serial.printf("Integrated seconds: %lu\n", static_cast<unsigned long>(d.halfIntegratedSec));
  Serial.printf("Complete: %s\n", complete ? "YES" : "NO");
  Serial.println("=================================");
}

static void appendDayLine(String& batch, size_t i, time_t timestamp, bool finalPreviousDay) {
  const DeviceState& d = states[i];
  uint32_t elapsedSec = 0;

  if (finalPreviousDay) {
    elapsedSec = 86400;
  } else if (timestamp > d.dayStart) {
    elapsedSec = static_cast<uint32_t>(timestamp - d.dayStart);
    if (elapsedSec > 86400) elapsedSec = 86400;
  }

  const bool coverageComplete = (d.dayIntegratedSec >= elapsedSec);

  char line[384];
  snprintf(line, sizeof(line),
           "switchbot_energy_day,device_id=%s,gateway=%s "
           "energy_today_wh=%.3f,integrated_seconds=%lui,elapsed_seconds=%lui,coverage_complete=%s %ld\n",
           d.deviceId.c_str(), GATEWAY_ID,
           d.dayWh,
           static_cast<unsigned long>(d.dayIntegratedSec),
           static_cast<unsigned long>(elapsedSec),
           coverageComplete ? "true" : "false",
           static_cast<long>(timestamp));
  batch += line;
}

// ============================================================
// Energy integration
// Uses previous measured power as constant until the next measurement.
// The interval is split exactly at xx:00 / xx:30 and at JST midnight.
// ============================================================
static void initializeWindows(DeviceState& d, time_t now) {
  if (d.halfStart == 0) {
    d.halfStart = getHalfHourStart(now);
    d.halfWh = 0.0;
    d.halfIntegratedSec = 0;
  }

  const time_t midnight = getLocalMidnight(now);
  if (d.dayStart == 0 || d.dayStart != midnight) {
    d.dayStart = midnight;
    d.dayWh = 0.0;
    d.dayIntegratedSec = 0;
  }
}

static void integrateUntil(size_t i, time_t now, String& batch) {
  DeviceState& d = states[i];
  initializeWindows(d, now);

  if (!d.haveLastSample) {
    return;
  }

  if (now <= d.lastEpoch) {
    return;
  }

  const uint32_t totalGapSec = static_cast<uint32_t>(now - d.lastEpoch);
  const bool validGap = (totalGapSec <= MAX_VALID_GAP_SEC);

  time_t cursor = d.lastEpoch;

  while (cursor < now) {
    const time_t halfEnd = d.halfStart + 1800;
    const time_t dayEnd = d.dayStart + 86400;

    time_t next = now;
    if (halfEnd < next) next = halfEnd;
    if (dayEnd < next) next = dayEnd;

    if (next <= cursor) {
      // Defensive recovery in case a restored timestamp is inconsistent.
      if (cursor >= halfEnd) {
        d.halfStart = getHalfHourStart(cursor);
        d.halfWh = 0.0;
        d.halfIntegratedSec = 0;
      }
      if (cursor >= dayEnd) {
        d.dayStart = getLocalMidnight(cursor);
        d.dayWh = 0.0;
        d.dayIntegratedSec = 0;
      }
      continue;
    }

    const uint32_t dt = static_cast<uint32_t>(next - cursor);
    if (validGap) {
      const double wh = static_cast<double>(d.lastPowerW) * static_cast<double>(dt) / 3600.0;
      d.halfWh += wh;
      d.halfIntegratedSec += dt;
      d.dayWh += wh;
      d.dayIntegratedSec += dt;
    }

    cursor = next;

    const bool hitHalfBoundary = (cursor == halfEnd);
    const bool hitDayBoundary = (cursor == dayEnd);

    if (hitHalfBoundary) {
      appendHalfHourLine(batch, i, halfEnd);
    }

    if (hitDayBoundary) {
      // Store the final value of the previous day at 23:59:59.
      appendDayLine(batch, i, dayEnd - 1, true);

      d.dayStart = dayEnd;
      d.dayWh = 0.0;
      d.dayIntegratedSec = 0;

      // Explicit reset point at 00:00:00 for Grafana.
      appendDayLine(batch, i, dayEnd, false);
    } else if (hitHalfBoundary) {
      // Normal hh:00 / hh:30 daily cumulative point.
      appendDayLine(batch, i, halfEnd, false);
    }

    if (hitHalfBoundary) {
      d.halfStart = halfEnd;
      d.halfWh = 0.0;
      d.halfIntegratedSec = 0;
      persistRequested = true;
    }

    if (hitDayBoundary) {
      persistRequested = true;
    }
  }
}

static void processPendingSamples(String& batch) {
  for (size_t i = 0; i < DEVICE_COUNT; ++i) {
    Sample& s = states[i].pending;
    if (!s.valid) {
      continue;
    }

    DeviceState& d = states[i];

    integrateUntil(i, s.epoch, batch);

    d.currentPowerW = s.powerW;
    d.currentRssi = s.rssi;
    d.currentStateOn = s.stateOn;
    d.currentSequence = s.sequence;
    d.currentEpoch = s.epoch;
    d.haveCurrent = true;

    appendInstantLine(batch, i, s);

    // The new sample becomes the power used for the next interval.
    d.lastPowerW = s.powerW;
    d.lastEpoch = s.epoch;
    d.haveLastSample = true;

    Serial.println();
    Serial.println("=================================");
    Serial.println("BLE measurement cycle");
    Serial.printf("Device ID : %s\n", d.deviceId.c_str());
    Serial.printf("MAC       : %s\n", DEVICE_CONFIG[i].mac);
    Serial.printf("RSSI      : %d dBm\n", s.rssi);
    Serial.printf("State     : %s\n", s.stateOn ? "ON" : "OFF");
    Serial.printf("Power     : %.1f W\n", s.powerW);
    Serial.printf("Sequence  : %u\n", static_cast<unsigned>(s.sequence));
    Serial.printf("Time      : %s JST\n", formatLocalTime(s.epoch).c_str());
    Serial.println("=================================");

    s.valid = false;
  }
}

// ============================================================
// Persistent daily accumulator checkpoint
// Checkpoint is written at every 30-minute boundary.
// After an unexpected reboot, completed half-hour contributions survive.
// The current unfinished 30-minute interval is intentionally not guessed.
// ============================================================
static void makeKey(char* out, size_t outLen, size_t index, const char* suffix) {
  snprintf(out, outLen, "d%u%s", static_cast<unsigned>(index), suffix);
}

static void loadPersistentState(time_t now) {
  const time_t todayStart = getLocalMidnight(now);

  for (size_t i = 0; i < DEVICE_COUNT; ++i) {
    char key[16];

    makeKey(key, sizeof(key), i, "day");
    const uint32_t savedDay = preferences.getULong(key, 0);

    if (static_cast<time_t>(savedDay) == todayStart) {
      states[i].dayStart = todayStart;

      makeKey(key, sizeof(key), i, "wh");
      states[i].dayWh = preferences.getDouble(key, 0.0);

      makeKey(key, sizeof(key), i, "sec");
      states[i].dayIntegratedSec = preferences.getULong(key, 0);

      Serial.printf("Restored day accumulator: %s %.3f Wh, %lu s\n",
                    states[i].deviceId.c_str(), states[i].dayWh,
                    static_cast<unsigned long>(states[i].dayIntegratedSec));
    } else {
      states[i].dayStart = todayStart;
      states[i].dayWh = 0.0;
      states[i].dayIntegratedSec = 0;
    }
  }
}

static void savePersistentState() {
  for (size_t i = 0; i < DEVICE_COUNT; ++i) {
    char key[16];

    makeKey(key, sizeof(key), i, "day");
    preferences.putULong(key, static_cast<uint32_t>(states[i].dayStart));

    makeKey(key, sizeof(key), i, "wh");
    preferences.putDouble(key, states[i].dayWh);

    makeKey(key, sizeof(key), i, "sec");
    preferences.putULong(key, states[i].dayIntegratedSec);
  }

  Serial.println("Daily accumulator checkpoint saved to NVS.");
}

// ============================================================
// Network / InfluxDB
// ============================================================
static bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println("Wi-Fi reconnecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected: %s, RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  Serial.println("Wi-Fi reconnect failed.");
  return false;
}

static bool writeInflux(const String& batch) {
  if (batch.length() == 0) {
    return true;
  }

  if (!ensureWiFi()) {
    return false;
  }

  WiFiClientSecure client;
  // Same behavior as the previous working setup: TLS certificate verification disabled.
  // For production, replace with CA certificate verification when convenient.
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(12000);

  if (!http.begin(client, INFLUX_WRITE_URL)) {
    Serial.println("InfluxDB HTTP begin failed.");
    return false;
  }

  http.addHeader("Authorization", String("Token ") + INFLUX_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  http.addHeader("Accept", "application/json");

  Serial.println();
  Serial.println("InfluxDB batch:");
  Serial.print(batch);

  const int code = http.POST(batch);

  if (code == 204) {
    Serial.println("InfluxDB write OK");
    http.end();
    return true;
  }

  Serial.printf("InfluxDB write failed. HTTP status: %d\n", code);
  if (code > 0) {
    Serial.println(http.getString());
  }
  http.end();
  return false;
}

// ============================================================
// Display
// ============================================================
static void updateDisplay() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(8, 8);
  M5.Lcd.printf("%s  3 plugs\n", GATEWAY_ID);

  for (size_t i = 0; i < DEVICE_COUNT; ++i) {
    M5.Lcd.setCursor(8, 42 + static_cast<int>(i) * 58);
    const String tail = states[i].deviceId.length() >= 4
                            ? states[i].deviceId.substring(states[i].deviceId.length() - 4)
                            : states[i].deviceId;

    if (states[i].haveCurrent) {
      M5.Lcd.printf("%s %s %.1fW\n", DEVICE_CONFIG[i].label, tail.c_str(), states[i].currentPowerW);
      M5.Lcd.setCursor(8, 64 + static_cast<int>(i) * 58);
      M5.Lcd.printf("Day %.2fWh RSSI %d", states[i].dayWh, states[i].currentRssi);
    } else {
      M5.Lcd.printf("%s %s waiting...", DEVICE_CONFIG[i].label, tail.c_str());
    }
  }
}

// ============================================================
// Setup / loop
// ============================================================
static void syncClock() {
  // POSIX TZ: JST = UTC+9, written as JST-9.
  configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org", "time.google.com");

  Serial.print("NTP sync");
  const uint32_t start = millis();
  while (time(nullptr) < 1700000000 && millis() - start < 30000) {
    Serial.print('.');
    delay(500);
  }
  Serial.println();

  const time_t now = time(nullptr);
  Serial.printf("Clock: %s JST (%ld)\n", formatLocalTime(now).c_str(), static_cast<long>(now));
}

void setup() {
  M5.begin();
  Serial.begin(115200);
  delay(500);

  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(8, 8);
  M5.Lcd.println("SwitchBot -> InfluxDB");
  M5.Lcd.println("3-plug energy logger");

  for (size_t i = 0; i < DEVICE_COUNT; ++i) {
    states[i].deviceId = macToDeviceId(DEVICE_CONFIG[i].mac);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  ensureWiFi();
  syncClock();

  preferences.begin("sbenergy", false);
  loadPersistentState(time(nullptr));

  BLEDevice::init("");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(&advCallbacks, true);
  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(90);

  updateDisplay();
}

void loop() {
  const uint32_t cycleStart = millis();

  // Clear only the per-cycle pending flags.
  for (size_t i = 0; i < DEVICE_COUNT; ++i) {
    states[i].pending.valid = false;
  }

  Serial.println();
  Serial.println("Scanning SwitchBot Plug Mini advertisements...");
  bleScan->start(BLE_SCAN_SECONDS, false);
  bleScan->clearResults();

  String batch;
  batch.reserve(4096);

  processPendingSamples(batch);

  // If a target was not seen this cycle, no interval is integrated for that target yet.
  // A later sample may integrate the gap only when it is <= MAX_VALID_GAP_SEC.

  const bool writeOk = writeInflux(batch);
  if (!writeOk) {
    Serial.println("InfluxDB batch write was not accepted.");
  }

  if (persistRequested) {
    savePersistentState();
    persistRequested = false;
  }

  updateDisplay();

  const uint32_t elapsed = millis() - cycleStart;
  if (elapsed < CYCLE_MS) {
    delay(CYCLE_MS - elapsed);
  }
}
