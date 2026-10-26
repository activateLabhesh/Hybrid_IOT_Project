#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#define WX_HOURS 12
#define WX_DAYS 3

struct WeatherData {
  bool valid = false;
  uint32_t fetchedEpoch = 0;
  uint32_t fetchedMs = 0;
  char error[48] = "";

  // Current conditions
  float tempC = NAN;
  float humidity = NAN;
  float windKmh = NAN;
  float precipNow = 0;
  int code = -1;

  // Next 12 hours (index 0 = current hour)
  uint8_t hProb[WX_HOURS] = {0};
  float hMm[WX_HOURS] = {0};

  // Daily
  float dMax[WX_DAYS], dMin[WX_DAYS], dRain[WX_DAYS], dEt0[WX_DAYS];
  uint8_t dProb[WX_DAYS];
  int dCode[WX_DAYS];
};

extern WeatherData weather;

void weatherBegin();
void weatherRequestRefresh();

// Is meaningful rain expected within `hours`? Fills probability, mm and ETA.
bool weatherRainExpected(uint8_t hours, uint8_t minProb, float minMm,
                         uint8_t* outProb, float* outMm, int* outEtaH);
bool weatherFresh();
void weatherToJson(JsonObject o);
