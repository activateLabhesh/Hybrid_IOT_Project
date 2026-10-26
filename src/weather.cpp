// Weather forecast from Open-Meteo (https://open-meteo.com) — free, no API key.
// Runs in its own FreeRTOS task so a slow HTTPS request never blocks the
// pump/safety logic in the main loop.
#include "weather.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "settings.h"
#include "state.h"

WeatherData weather;
static TaskHandle_t wxTask = nullptr;

static bool fetchWeather() {
  char url[512];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,precipitation,weather_code,wind_speed_10m"
           "&hourly=precipitation_probability,precipitation&forecast_hours=%d"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,"
           "precipitation_probability_max,et0_fao_evapotranspiration"
           "&forecast_days=%d&timezone=auto",
           settings.lat, settings.lon, WX_HOURS, WX_DAYS);

  WiFiClientSecure client;
  client.setInsecure();  // public, read-only data; avoids shipping a CA bundle
  HTTPClient http;
  http.setTimeout(15000);
  http.useHTTP10(true);  // no chunked encoding -> stream straight into the parser
  if (!http.begin(client, url)) {
    Guard g;
    strlcpy(weather.error, "connect failed", sizeof(weather.error));
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    Guard g;
    snprintf(weather.error, sizeof(weather.error), "HTTP %d", code);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Guard g;
    snprintf(weather.error, sizeof(weather.error), "JSON: %s", err.c_str());
    return false;
  }

  WeatherData w;
  JsonObject cur = doc["current"];
  w.tempC = cur["temperature_2m"] | NAN;
  w.humidity = cur["relative_humidity_2m"] | NAN;
  w.windKmh = cur["wind_speed_10m"] | NAN;
  w.precipNow = cur["precipitation"] | 0.0f;
  w.code = cur["weather_code"] | -1;

  JsonArray hp = doc["hourly"]["precipitation_probability"];
  JsonArray hm = doc["hourly"]["precipitation"];
  for (int i = 0; i < WX_HOURS; i++) {
    w.hProb[i] = hp[i] | 0;
    w.hMm[i] = hm[i] | 0.0f;
  }

  JsonObject d = doc["daily"];
  for (int i = 0; i < WX_DAYS; i++) {
    w.dCode[i] = d["weather_code"][i] | -1;
    w.dMax[i] = d["temperature_2m_max"][i] | NAN;
    w.dMin[i] = d["temperature_2m_min"][i] | NAN;
    w.dRain[i] = d["precipitation_sum"][i] | 0.0f;
    w.dProb[i] = d["precipitation_probability_max"][i] | 0;
    w.dEt0[i] = d["et0_fao_evapotranspiration"][i] | 0.0f;
  }

  int32_t offset = doc["utc_offset_seconds"] | 0;
  w.valid = true;
  w.fetchedEpoch = nowEpoch();
  w.fetchedMs = millis();

  {
    Guard g;
    weather = w;
  }

  // Learn the farm's timezone from the forecast location
  if (offset != settings.tzOffsetSec) {
    settings.tzOffsetSec = offset;
    settingsSave();
    configTime(offset, 0, "pool.ntp.org", "time.google.com");
    eventLog(EV_INFO, "Timezone set to UTC%+.1f from weather location", offset / 3600.0f);
  }
  return true;
}

static void weatherTask(void*) {
  uint32_t lastOk = 0;
  bool hadError = false;
  for (;;) {
    uint32_t waitMs = 60000;  // retry in 1 min after failure / without WiFi
    if (settings.weatherEnabled && WiFi.status() == WL_CONNECTED) {
      if (fetchWeather()) {
        if (hadError || lastOk == 0) eventLog(EV_INFO, "Weather forecast updated");
        hadError = false;
        lastOk = millis();
        waitMs = settings.weatherIntervalMin * 60000UL;
      } else if (!hadError) {
        hadError = true;
        eventLog(EV_WARN, "Weather update failed: %s", weather.error);
      }
    }
    // Sleep until the interval passes or someone asks for a refresh
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
  }
}

void weatherBegin() {
  xTaskCreatePinnedToCore(weatherTask, "weather", 12288, nullptr, 1, &wxTask, 0);
}

void weatherRequestRefresh() {
  if (wxTask) xTaskNotifyGive(wxTask);
}

bool weatherFresh() {
  return weather.valid && millis() - weather.fetchedMs < WEATHER_STALE_S * 1000UL;
}

bool weatherRainExpected(uint8_t hours, uint8_t minProb, float minMm,
                         uint8_t* outProb, float* outMm, int* outEtaH) {
  if (!weatherFresh()) return false;
  // Index 0 is the hour the forecast was fetched in; skip the hours already passed.
  int elapsedH = (millis() - weather.fetchedMs) / 3600000UL;
  uint8_t maxProb = 0;
  float mm = 0;
  int eta = -1;
  for (int i = elapsedH; i < WX_HOURS && i < elapsedH + hours; i++) {
    mm += weather.hMm[i];
    if (weather.hProb[i] > maxProb) maxProb = weather.hProb[i];
    if (eta < 0 && weather.hProb[i] >= minProb) eta = i - elapsedH;
  }
  if (outProb) *outProb = maxProb;
  if (outMm) *outMm = mm;
  if (outEtaH) *outEtaH = eta;
  return maxProb >= minProb && mm >= minMm;
}

void weatherToJson(JsonObject o) {
  Guard g;
  o["valid"] = weather.valid;
  o["fresh"] = weatherFresh();
  o["fetched"] = weather.fetchedEpoch;
  o["ageMin"] = weather.valid ? (millis() - weather.fetchedMs) / 60000 : -1;
  o["error"] = weather.error;
  if (!weather.valid) return;
  o["temp"] = weather.tempC;
  o["hum"] = weather.humidity;
  o["wind"] = weather.windKmh;
  o["precip"] = weather.precipNow;
  o["code"] = weather.code;
  JsonArray hp = o["hProb"].to<JsonArray>();
  JsonArray hm = o["hMm"].to<JsonArray>();
  for (int i = 0; i < WX_HOURS; i++) {
    hp.add(weather.hProb[i]);
    hm.add(weather.hMm[i]);
  }
  JsonArray days = o["days"].to<JsonArray>();
  for (int i = 0; i < WX_DAYS; i++) {
    JsonObject d = days.add<JsonObject>();
    d["code"] = weather.dCode[i];
    d["max"] = weather.dMax[i];
    d["min"] = weather.dMin[i];
    d["rain"] = weather.dRain[i];
    d["prob"] = weather.dProb[i];
    d["et0"] = weather.dEt0[i];
  }
}
