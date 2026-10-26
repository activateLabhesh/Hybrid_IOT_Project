// Web portal + REST API + WebSocket live updates.
//
//   GET  /api/status     live snapshot (also pushed on ws://<host>/ws every 2 s)
//   GET  /api/history    24 h of 5-minute samples + 14 days water use
//   GET  /api/events     event log
//   GET  /api/settings
//   POST /api/settings   (auth) JSON with any settings keys
//   POST /api/control    (auth) {"cmd": "..."} — see handleControl()
//   POST /api/login      (auth) check password
//   POST /api/ota        (auth) firmware .bin upload
//
// Auth = HTTP Basic, user "admin", password from settings (default admin123).
#include "web.h"
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <WiFi.h>
#include "config.h"
#include "irrigation.h"
#include "net.h"
#include "sensors.h"
#include "settings.h"
#include "state.h"
#include "weather.h"
#include "web_assets.h"

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static uint32_t lastPushMs = 0, lastCleanMs = 0, rebootAtMs = 0;

static bool authed(AsyncWebServerRequest* r) {
  if (r->authenticate("admin", settings.adminPass)) return true;
  // Plain 401 without WWW-Authenticate so the browser shows our own login form
  r->send(401, "application/json", "{\"error\":\"Wrong password\"}");
  return false;
}

static void sendJson(AsyncWebServerRequest* r, JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  r->send(code, "application/json", out);
}

static void sendError(AsyncWebServerRequest* r, int code, const String& msg) {
  JsonDocument d;
  d["error"] = msg;
  sendJson(r, d, code);
}

static void buildStatus(JsonDocument& doc) {
  Guard g;
  uint32_t now = millis();
  doc["farm"] = settings.farmName;
  doc["fw"] = FW_VERSION;
  doc["uptime"] = now / 1000;
  doc["time"] = nowEpoch();
  doc["tz"] = settings.tzOffsetSec;
  doc["heap"] = ESP.getFreeHeap();

  JsonObject s = doc["sensors"].to<JsonObject>();
  s["moisture"] = sensors.moisturePct;
  s["soilRaw"] = sensors.soilRaw;
  s["soilOk"] = sensors.soilOk;
  s["raining"] = sensors.raining;
  s["rainRaw"] = sensors.rainRaw;
  s["rainIntensity"] = sensors.rainIntensity;
  s["rainAgoMin"] = sensors.lastRainMs ? (int32_t)((now - sensors.lastRainMs) / 60000) : -1;
  if (sensors.dhtOk) {
    s["temp"] = sensors.tempC;
    s["hum"] = sensors.humidity;
  }
  s["dhtOk"] = sensors.dhtOk;
  s["tankPct"] = sensors.tankPct;
  s["tankL"] = sensors.tankLitres;
  s["tankCap"] = settings.tankCapacityL;
  s["distance"] = sensors.distanceCm;
  s["tankOk"] = sensors.tankOk;
  s["flow"] = sensors.flowLpm;

  irrigationToJson(doc["irr"].to<JsonObject>());

  JsonObject st = doc["stats"].to<JsonObject>();
  st["total"] = stats.totalLitres;
  st["today"] = stats.todayLitres;
  st["saved"] = stats.savedLitres;
  st["avgCycle"] = stats.avgCycleLitres;
  st["hits"] = stats.forecastHits;
  st["misses"] = stats.forecastMisses;
  st["skips"] = stats.rainSkips;
  st["pumpHours"] = stats.pumpSeconds / 3600.0f;
  st["yesterday"] = stats.dailyLitres[0];

  netToJson(doc["net"].to<JsonObject>());
  weatherToJson(doc["weather"].to<JsonObject>());
}

static void handleControl(AsyncWebServerRequest* r, JsonVariant& json) {
  if (!authed(r)) return;
  JsonObject b = json.as<JsonObject>();
  String cmd = b["cmd"] | "";
  String err;
  bool ok = true;
  {
    Guard g;
    if (cmd == "pump_on") {
      ok = irrigationManualStart(b["minutes"] | 0.0f, b["litres"] | 0.0f, err);
    } else if (cmd == "pump_off") {
      irrigationStop("stopped from portal");
    } else if (cmd == "mode") {
      irrigationSetMode(strcmp(b["mode"] | "auto", "manual") == 0 ? MODE_MANUAL : MODE_AUTO);
    } else if (cmd == "pause") {
      irrigationPause(b["hours"] | 6.0f);
    } else if (cmd == "resume") {
      irrigationResume();
    } else if (cmd == "ack") {
      irrigationAckAlarms();
    } else if (cmd == "weather_refresh") {
      weatherRequestRefresh();
    } else if (cmd == "calib_soil_dry") {
      settings.soilDryRaw = sensors.soilRaw;
      settingsSave();
      eventLog(EV_INFO, "Soil calibrated: DRY = %d", sensors.soilRaw);
    } else if (cmd == "calib_soil_wet") {
      settings.soilWetRaw = sensors.soilRaw;
      settingsSave();
      eventLog(EV_INFO, "Soil calibrated: WET = %d", sensors.soilRaw);
    } else if (cmd == "calib_tank_empty") {
      if (sensors.tankOk) {
        settings.tankEmptyCm = sensors.distanceCm;
        settingsSave();
        eventLog(EV_INFO, "Tank calibrated: EMPTY = %.1f cm", sensors.distanceCm);
      } else { ok = false; err = "Tank sensor not reading"; }
    } else if (cmd == "calib_tank_full") {
      if (sensors.tankOk) {
        settings.tankFullCm = sensors.distanceCm;
        settingsSave();
        eventLog(EV_INFO, "Tank calibrated: FULL = %.1f cm", sensors.distanceCm);
      } else { ok = false; err = "Tank sensor not reading"; }
    } else if (cmd == "reset_stats") {
      statsReset();
      eventLog(EV_INFO, "Statistics reset");
    } else if (cmd == "reboot") {
      irrigationStop("reboot");
      rebootAtMs = millis() + 1000;
    } else {
      ok = false;
      err = "Unknown command";
    }
  }
  if (!ok) return sendError(r, 400, err);
  JsonDocument d;
  d["ok"] = true;
  sendJson(r, d);
}

static void handleSettings(AsyncWebServerRequest* r, JsonVariant& json) {
  if (!authed(r)) return;
  bool wifiChanged;
  {
    Guard g;
    wifiChanged = settingsFromJson(json.as<JsonObjectConst>());
    settingsSave();
  }
  eventLog(EV_INFO, "Settings updated%s", wifiChanged ? " — rebooting to join new WiFi" : "");
  weatherRequestRefresh();  // location may have changed
  if (wifiChanged) rebootAtMs = millis() + 1500;
  JsonDocument d;
  d["ok"] = true;
  d["reboot"] = wifiChanged;
  sendJson(r, d);
}

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*, uint8_t*,
                      size_t) {
  if (type == WS_EVT_CONNECT) {
    JsonDocument doc;
    buildStatus(doc);
    String out;
    serializeJson(doc, out);
    client->text(out);
  }
}

void webBegin() {
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    const WebAsset* a = &WEB_ASSETS[i];
    server.on(a->path, HTTP_GET, [a](AsyncWebServerRequest* r) {
      AsyncWebServerResponse* res = r->beginResponse(200, a->mime, a->data, a->len);
      res->addHeader("Content-Encoding", "gzip");
      res->addHeader("Cache-Control", "no-cache");
      r->send(res);
    });
  }

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument doc;
    buildStatus(doc);
    sendJson(r, doc);
  });
  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument doc;
    historyToJson(doc.to<JsonObject>());
    sendJson(r, doc);
  });
  server.on("/api/events", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument doc;
    eventsToJson(doc.to<JsonArray>());
    sendJson(r, doc);
  });
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument doc;
    {
      Guard g;
      settingsToJson(doc.to<JsonObject>(), false);
    }
    sendJson(r, doc);
  });
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!authed(r)) return;
    r->send(200, "application/json", "{\"ok\":true}");
  });

  auto* ctl = new AsyncCallbackJsonWebHandler("/api/control", handleControl);
  ctl->setMethod(HTTP_POST);
  server.addHandler(ctl);
  auto* cfg = new AsyncCallbackJsonWebHandler("/api/settings", handleSettings);
  cfg->setMethod(HTTP_POST);
  server.addHandler(cfg);

  // Firmware update over WiFi (upload .pio/build/esp32dev/firmware.bin)
  server.on(
      "/api/ota", HTTP_POST,
      [](AsyncWebServerRequest* r) {
        if (!r->authenticate("admin", settings.adminPass)) return (void)r->send(401, "application/json", "{\"error\":\"Wrong password\"}");
        bool ok = !Update.hasError();
        r->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"Update failed\"}");
        if (ok) rebootAtMs = millis() + 1500;
      },
      [](AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (!r->authenticate("admin", settings.adminPass)) return;
        if (index == 0) {
          {
            Guard g;
            irrigationStop("firmware update");
          }
          eventLog(EV_INFO, "Firmware update started (%s)", filename.c_str());
          Update.begin(UPDATE_SIZE_UNKNOWN);
        }
        if (Update.write(data, len) != len) Update.printError(Serial);
        if (final) {
          if (Update.end(true)) eventLog(EV_INFO, "Firmware update OK — rebooting");
          else eventLog(EV_ALARM, "Firmware update failed");
        }
      });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.onNotFound([](AsyncWebServerRequest* r) {
    if (netApActive() && !r->url().startsWith("/api")) {
      // captive portal: any URL on the hotspot opens the dashboard
      r->redirect("http://" + WiFi.softAPIP().toString() + "/");
      return;
    }
    r->send(404, "text/plain", "Not found");
  });

  server.begin();
}

void webLoop() {
  uint32_t now = millis();
  if (now - lastCleanMs > 1000) {
    lastCleanMs = now;
    ws.cleanupClients();
  }
  if (now - lastPushMs > WS_PUSH_INTERVAL_MS && ws.count() > 0) {
    lastPushMs = now;
    JsonDocument doc;
    buildStatus(doc);
    String out;
    serializeJson(doc, out);
    ws.textAll(out);
  }
  if (rebootAtMs && (int32_t)(now - rebootAtMs) >= 0) {
    statsSave();
    ESP.restart();
  }
}
