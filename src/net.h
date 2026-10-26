#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

void netBegin();
void netLoop();
bool netApActive();
void netToJson(JsonObject o);
