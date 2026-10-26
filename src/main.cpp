// Hybrid-Flow — smart, weather-aware farm irrigation controller for ESP32.
//
// The controller is fully autonomous: all irrigation and safety decisions are
// made locally, so the farm keeps being watered correctly even if WiFi or the
// internet is down. The internet only adds the weather forecast.
#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "irrigation.h"
#include "net.h"
#include "sensors.h"
#include "settings.h"
#include "state.h"
#include "weather.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== Hybrid-Flow v%s ===\n", FW_VERSION);

  stateBegin();
  settingsLoad();
  statsLoad();
  irrigationBegin();   // pump OFF as early as possible
  sensorsBegin();
  eventLog(EV_INFO, "Controller started (fw %s)", FW_VERSION);

  netBegin();
  webBegin();
  weatherBegin();
}

// One status line on the serial monitor every 10 s: proves the controller is
// alive and makes sensor wiring easy to check without the portal.
static void serialHeartbeat() {
  static uint32_t last = 0;
  if (millis() - last < 10000) return;
  last = millis();
  Guard g;
  Serial.printf("[%5lus] soil %3.0f%% (raw %4d%s) | rain %s (raw %4d) | air %s | tank %s | pump %s | clients %d | heap %u\n",
                (unsigned long)(millis() / 1000), sensors.moisturePct, sensors.soilRaw, sensors.soilOk ? "" : " FAULT",
                sensors.raining ? "YES" : "no", sensors.rainRaw,
                sensors.dhtOk ? String(String(sensors.tempC, 1) + "C " + String(sensors.humidity, 0) + "%").c_str() : "DHT FAULT",
                sensors.tankOk ? String(String(sensors.tankPct, 0) + "% (" + String(sensors.distanceCm, 1) + " cm)").c_str() : "NO ECHO",
                irrigationPumpOn() ? "ON" : "off", WiFi.softAPgetStationNum(), ESP.getFreeHeap());
  Serial.printf("        relay pin D%d = %s | relay type: %s-trigger | mode %s\n", PIN_PUMP,
                irrigationRelayPinHigh() ? "HIGH (3.3V)" : "LOW (0V)", settings.relayActiveLow ? "LOW" : "HIGH",
                settings.mode == MODE_AUTO ? "AUTO" : "MANUAL");
}

// Serial commands: 't' = relay blink test for 3 min, 'x' = stop it
static void serialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    Guard g;
    if (c == 't') {
      Serial.println("RELAY TEST: toggling relay every 2 s for 3 minutes (type x to stop)");
      irrigationRelayTest(180);
    } else if (c == 'x') {
      irrigationRelayTest(0);
    }
  }
}

void loop() {
  netLoop();
  sensorsLoop();
  irrigationLoop();
  statsLoop();
  historyLoop();
  webLoop();
  serialHeartbeat();
  serialCommands();
  delay(5);
}
