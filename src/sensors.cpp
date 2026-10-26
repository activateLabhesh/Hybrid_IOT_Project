#include "sensors.h"
#include <DHT.h>
#include "config.h"
#include "settings.h"
#include "state.h"

SensorData sensors;

static DHT dht(PIN_DHT, DHT11);
static uint32_t lastDhtMs = 0, lastTankMs = 0, lastSoilRainMs = 0;
static uint8_t dhtFails = 0, tankFails = 0;
static uint8_t rainWetCount = 0, rainDryCount = 0;
static bool tankFirst = true;

void sensorsBegin() {
  pinMode(PIN_RAIN_DO, INPUT_PULLUP);
  pinMode(PIN_US_TRIG, OUTPUT);
  digitalWrite(PIN_US_TRIG, LOW);
  pinMode(PIN_US_ECHO, INPUT);

  analogReadResolution(12);  // default attenuation (11 dB) reads the full 0-3.3 V range

  dht.begin();
}

// Average of `samples` reads; `spread` = max - min, used to spot a floating input
static int readAnalogAvg(uint8_t pin, int samples = 16, int* spread = nullptr) {
  uint32_t sum = 0;
  int lo = 4095, hi = 0;
  for (int i = 0; i < samples; i++) {
    int v = analogRead(pin);
    sum += v;
    lo = min(lo, v);
    hi = max(hi, v);
    delayMicroseconds(200);
  }
  if (spread) *spread = hi - lo;
  return sum / samples;
}

static void sampleSoil() {
  int spread;
  int soil = readAnalogAvg(PIN_SOIL, 16, &spread);

  Guard g;
  sensors.soilRaw = soil;
  // A resistive probe legitimately reads ~4095 in bone-dry soil, so only a
  // dead module (~0) or a loose AO wire (wildly jumping readings) is a fault.
  // A probe pulled out of the soil is caught by the irrigation "no response" check.
  sensors.soilOk = soil > 50 && spread < 1500;
  int dry = settings.soilDryRaw, wet = settings.soilWetRaw;
  if (dry != wet) {
    float pct = (float)(dry - soil) * 100.0f / (float)(dry - wet);
    sensors.moisturePct = constrain(pct, 0.0f, 100.0f);
  }
}

static void sampleRain() {
  int rain = readAnalogAvg(PIN_RAIN_AO);
  bool rainDo = digitalRead(PIN_RAIN_DO) == LOW;

  Guard g;
  sensors.rainRaw = rain;
  int intensity = map(constrain(rain, 1000, 4095), 4095, 1000, 0, 100);
  sensors.rainIntensity = constrain(intensity, 0, 100);
  bool wetNow = rain < settings.rainWetRaw || rainDo;

  // Debounce: 2 wet samples to declare rain, 3 dry samples to clear it
  if (wetNow) {
    rainDryCount = 0;
    if (++rainWetCount >= 2 && !sensors.raining) {
      sensors.raining = true;
      eventLog(EV_WARN, "Rain detected by field sensor");
    }
  } else {
    rainWetCount = 0;
    if (++rainDryCount >= 3 && sensors.raining) {
      sensors.raining = false;
      eventLog(EV_INFO, "Rain stopped (field sensor)");
    }
  }
  if (sensors.raining) sensors.lastRainMs = millis();
}

static float readDistanceOnce(float speedCmPerUs) {
  digitalWrite(PIN_US_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_US_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_US_TRIG, LOW);
  unsigned long us = pulseIn(PIN_US_ECHO, HIGH, 30000);
  if (us == 0) return NAN;
  return us * speedCmPerUs / 2.0f;
}

static void sampleTank() {
  // Speed of sound depends on air temperature — use the DHT11 reading.
  float t = sensors.dhtOk ? sensors.tempC : 25.0f;
  float speed = (331.3f + 0.606f * t) / 10000.0f;  // cm/us

  float r[5];
  int n = 0;
  for (int i = 0; i < 5; i++) {
    float d = readDistanceOnce(speed);
    if (!isnan(d) && d > 2 && d < 450) r[n++] = d;
    delay(15);
  }

  Guard g;
  if (n < 3) {
    if (++tankFails >= 5) sensors.tankOk = false;
    return;
  }
  tankFails = 0;
  // median rejects splashes / echoes
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (r[j] < r[i]) std::swap(r[i], r[j]);
  float d = r[n / 2];

  float span = settings.tankEmptyCm - settings.tankFullCm;
  float pct = span > 0 ? (settings.tankEmptyCm - d) * 100.0f / span : 0;
  pct = constrain(pct, 0.0f, 100.0f);

  sensors.distanceCm = d;
  sensors.tankPct = tankFirst ? pct : sensors.tankPct * 0.7f + pct * 0.3f;  // smooth ripples
  sensors.tankLitres = sensors.tankPct * settings.tankCapacityL / 100.0f;
  sensors.tankOk = true;
  tankFirst = false;
}

static void sampleDht() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  Guard g;
  if (isnan(t) || isnan(h)) {
    if (++dhtFails >= 5) sensors.dhtOk = false;
    return;
  }
  dhtFails = 0;
  sensors.tempC = t;
  sensors.humidity = h;
  sensors.dhtOk = true;
}

void sensorsLoop() {
  uint32_t now = millis();

  if (lastSoilRainMs == 0 || now - lastSoilRainMs >= SOIL_RAIN_INTERVAL_MS) {
    lastSoilRainMs = now;
    sampleSoil();
    sampleRain();
  }

  if (now - lastDhtMs >= DHT_INTERVAL_MS) {
    lastDhtMs = now;
    sampleDht();
  }
  if (now - lastTankMs >= TANK_INTERVAL_MS) {
    lastTankMs = now;
    sampleTank();
  }
}
