#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

enum RunSource : uint8_t { SRC_NONE = 0, SRC_AUTO = 1, SRC_MANUAL = 2 };

// Alarm bits
enum : uint32_t {
  AL_DRY_RUN = 1 << 0,     // pump on but tank level not dropping (latched until acknowledged)
  AL_LEAK = 1 << 1,        // tank level falling while pump is off (latched until acknowledged)
  AL_TANK_LOW = 1 << 2,
  AL_SOIL_FAULT = 1 << 3,
  AL_DHT_FAULT = 1 << 4,
  AL_TANK_FAULT = 1 << 5,
  AL_WEATHER = 1 << 6,     // forecast unavailable (rain sensor still protects)
  AL_SOIL_NO_RESPONSE = 1 << 7,  // watered to max run time but moisture never rose (latched)
};

void irrigationBegin();
void irrigationLoop();

// Commands (call with the global lock held — web handlers do)
bool irrigationManualStart(float minutes, float litres, String& err);
void irrigationStop(const char* why);
void irrigationSetMode(uint8_t mode);
void irrigationPause(float hours);
void irrigationResume();
void irrigationAckAlarms();

void irrigationToJson(JsonObject o);
bool irrigationPumpOn();

// Wiring check: toggles the relay pin every 2 s for `seconds` (pump must be off).
// Started from the serial monitor by typing 't'; 'x' stops it.
void irrigationRelayTest(uint16_t seconds);
bool irrigationRelayTestActive();
bool irrigationRelayPinHigh();
