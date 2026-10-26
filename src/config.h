#pragma once
#include <Arduino.h>

#define FW_VERSION "1.0.0"

// ---------------------------------------------------------------------------
// Pin map (see README.md for the wiring diagram)
// All signals are on ONE side of the ESP32 DevKit (D34 D35 D32 D33 D25 D26 D27 D14 D12 D13).
// Analog sensors must use ADC1 (D32-D35) — ADC2 pins do not work with WiFi on.
// D34/D35 are input-only. D12 is left free: it must be LOW at boot.
// Spare: D14, D26.
// ---------------------------------------------------------------------------
#define PIN_SOIL        34   // resistive soil probe module AO (analog, higher = drier)
#define PIN_RAIN_AO     35   // rain-drop module AO (analog, lower = wetter)
#define PIN_RAIN_DO     32   // rain-drop module DO (LOW = wet)
#define PIN_DHT         27   // DHT11 data
#define PIN_US_TRIG     25   // HC-SR04 TRIG
#define PIN_US_ECHO     33   // HC-SR04 ECHO  (5V -> use 1k/2k voltage divider!)
#define PIN_PUMP        13   // relay module IN (relay type LOW/HIGH-trigger is set in the portal)
#define PIN_LED         2    // on-board LED (status)

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define SOIL_RAIN_INTERVAL_MS   2000     // soil + rain modules are powered continuously from 3V3
#define DHT_INTERVAL_MS         3000
#define TANK_INTERVAL_MS        2000
#define CONTROL_INTERVAL_MS     1000
#define HISTORY_INTERVAL_S      300      // one chart point every 5 min (24h in RAM)
#define HISTORY_SIZE            288
#define EVENT_LOG_SIZE          50
#define WS_PUSH_INTERVAL_MS     2000

// Safety limits that cannot be changed from the portal
#define HARD_MAX_RUN_MIN        180      // absolute cap for any single pump run
#define PUMP_MIN_OFF_MS         15000    // anti short-cycling protection
#define TANK_HARD_FLOOR_PCT     3        // pump always stops below this
#define WEATHER_STALE_S         (3 * 3600)
#define BOOT_GRACE_MS           60000    // no automatic pump start right after power-up

#define AP_PASSWORD             "hybridflow"   // fallback hotspot password
#define MDNS_NAME               "hybridflow"   // http://hybridflow.local
