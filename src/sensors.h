#pragma once
#include <Arduino.h>

struct SensorData {
  // Soil
  int soilRaw = 0;
  float moisturePct = 0;
  bool soilOk = false;

  // Rain-drop sensor (debounced)
  int rainRaw = 4095;
  bool raining = false;
  uint8_t rainIntensity = 0;   // 0..100 %
  uint32_t lastRainMs = 0;     // millis() when rain was last seen (0 = never)

  // DHT11
  float tempC = NAN;
  float humidity = NAN;
  bool dhtOk = false;

  // HC-SR04 tank
  float distanceCm = NAN;
  float tankPct = 0;
  float tankLitres = 0;
  bool tankOk = false;

  // Water delivered — no flow sensor, so these are estimated by the irrigation
  // module from the pump's rated flow (settings.pumpLpm) while the pump runs.
  float flowLpm = 0;
  double litresSinceBoot = 0;
};

extern SensorData sensors;

void sensorsBegin();
void sensorsLoop();
