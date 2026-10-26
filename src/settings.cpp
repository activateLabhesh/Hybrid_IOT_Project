#include "settings.h"
#include <Preferences.h>
#include "config.h"

static const uint32_t SETTINGS_MAGIC = 0x48464C35;  // "HFL5" — bump when struct changes

Settings settings;

void settingsDefaults(Settings& s) {
  memset(&s, 0, sizeof(s));
  s.magic = SETTINGS_MAGIC;
  strlcpy(s.adminPass, "admin123", sizeof(s.adminPass));
  strlcpy(s.farmName, "My Farm", sizeof(s.farmName));
  s.lat = 28.61f;  // change from the portal ("Use my location")
  s.lon = 77.21f;
  s.tzOffsetSec = 0;

  s.moistStart = 35;
  s.moistStop = 60;
  s.moistCritical = 20;
  s.maxRunMin = 45;
  s.cycleSoak = true;
  s.cycleRunMin = 8;
  s.soakMin = 10;
  s.dailyLimitL = 0;

  s.tankMinPct = 10;
  s.tankEmptyCm = 100;
  s.tankFullCm = 10;
  s.tankCapacityL = 1000;

  s.weatherEnabled = true;
  s.rainProbPct = 60;
  s.rainMm = 2.0f;
  s.lookaheadH = 6;
  s.maxWaitH = 10;
  s.rainLockoutMin = 90;
  s.weatherIntervalMin = 30;

  s.windowEnabled = true;
  s.w1Start = 5;  s.w1End = 10;
  s.w2Start = 16; s.w2End = 20;

  s.heatBoost = true;
  s.heatTempC = 35;
  s.heatBoostPct = 5;

  s.soilDryRaw = 3800;   // resistive probe in dry field soil
  s.soilWetRaw = 1500;   // ... in freshly watered soil
  s.rainWetRaw = 2800;
  s.pumpLpm = 15;

  s.dryRunMin = 4;
  s.leakPct = 5;
  s.relayActiveLow = true;

  s.mode = MODE_MANUAL;  // first boot: no automatic pumping until the farmer calibrates and switches to AUTO
}

void settingsLoad() {
  Preferences p;
  p.begin("hf_cfg", true);
  size_t n = p.getBytesLength("s");
  bool ok = false;
  if (n == sizeof(Settings)) {
    p.getBytes("s", &settings, sizeof(Settings));
    ok = settings.magic == SETTINGS_MAGIC;
  }
  p.end();
  if (!ok) {
    settingsDefaults(settings);
    settingsSave();
  }
}

void settingsSave() {
  Preferences p;
  p.begin("hf_cfg", false);
  p.putBytes("s", &settings, sizeof(Settings));
  p.end();
}

void settingsToJson(JsonObject o, bool includeSecrets) {
  const Settings& s = settings;
  o["wifiSsid"] = s.wifiSsid;
  o["wifiPass"] = includeSecrets ? s.wifiPass : (s.wifiPass[0] ? "********" : "");
  o["farmName"] = s.farmName;
  o["lat"] = s.lat;
  o["lon"] = s.lon;
  o["moistStart"] = s.moistStart;
  o["moistStop"] = s.moistStop;
  o["moistCritical"] = s.moistCritical;
  o["maxRunMin"] = s.maxRunMin;
  o["cycleSoak"] = s.cycleSoak;
  o["cycleRunMin"] = s.cycleRunMin;
  o["soakMin"] = s.soakMin;
  o["dailyLimitL"] = s.dailyLimitL;
  o["tankMinPct"] = s.tankMinPct;
  o["tankEmptyCm"] = s.tankEmptyCm;
  o["tankFullCm"] = s.tankFullCm;
  o["tankCapacityL"] = s.tankCapacityL;
  o["weatherEnabled"] = s.weatherEnabled;
  o["rainProbPct"] = s.rainProbPct;
  o["rainMm"] = s.rainMm;
  o["lookaheadH"] = s.lookaheadH;
  o["maxWaitH"] = s.maxWaitH;
  o["rainLockoutMin"] = s.rainLockoutMin;
  o["weatherIntervalMin"] = s.weatherIntervalMin;
  o["windowEnabled"] = s.windowEnabled;
  o["w1Start"] = s.w1Start;
  o["w1End"] = s.w1End;
  o["w2Start"] = s.w2Start;
  o["w2End"] = s.w2End;
  o["heatBoost"] = s.heatBoost;
  o["heatTempC"] = s.heatTempC;
  o["heatBoostPct"] = s.heatBoostPct;
  o["soilDryRaw"] = s.soilDryRaw;
  o["soilWetRaw"] = s.soilWetRaw;
  o["rainWetRaw"] = s.rainWetRaw;
  o["pumpLpm"] = s.pumpLpm;
  o["dryRunMin"] = s.dryRunMin;
  o["leakPct"] = s.leakPct;
  o["relayActiveLow"] = s.relayActiveLow;
}

template <typename T>
static void take(JsonObjectConst o, const char* key, T& dst, T lo, T hi) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return;
  float val = v.as<float>();
  dst = (T)constrain(val, (float)lo, (float)hi);
}

static void take(JsonObjectConst o, const char* key, bool& dst, bool, bool) {
  JsonVariantConst v = o[key];
  if (v.is<bool>()) dst = v.as<bool>();
}

static void takeStr(JsonObjectConst o, const char* key, char* dst, size_t len) {
  JsonVariantConst v = o[key];
  if (!v.is<const char*>()) return;
  strlcpy(dst, v.as<const char*>(), len);
}

bool settingsFromJson(JsonObjectConst o) {
  Settings& s = settings;
  bool wifiChanged = false;

  if (o["wifiSsid"].is<const char*>() && strcmp(o["wifiSsid"], s.wifiSsid) != 0) {
    takeStr(o, "wifiSsid", s.wifiSsid, sizeof(s.wifiSsid));
    wifiChanged = true;
  }
  // The masked placeholder means "unchanged"
  if (o["wifiPass"].is<const char*>() && strcmp(o["wifiPass"], "********") != 0 &&
      strcmp(o["wifiPass"], s.wifiPass) != 0) {
    takeStr(o, "wifiPass", s.wifiPass, sizeof(s.wifiPass));
    wifiChanged = true;
  }
  if (o["adminPass"].is<const char*>() && strlen(o["adminPass"]) >= 4) {
    takeStr(o, "adminPass", s.adminPass, sizeof(s.adminPass));
  }
  takeStr(o, "farmName", s.farmName, sizeof(s.farmName));

  take<float>(o, "lat", s.lat, -90, 90);
  take<float>(o, "lon", s.lon, -180, 180);
  take<uint8_t>(o, "moistStart", s.moistStart, 1, 95);
  take<uint8_t>(o, "moistStop", s.moistStop, 2, 100);
  take<uint8_t>(o, "moistCritical", s.moistCritical, 0, 90);
  take<uint16_t>(o, "maxRunMin", s.maxRunMin, 1, HARD_MAX_RUN_MIN);
  take(o, "cycleSoak", s.cycleSoak, false, true);
  take<uint8_t>(o, "cycleRunMin", s.cycleRunMin, 1, 120);
  take<uint8_t>(o, "soakMin", s.soakMin, 1, 120);
  take<float>(o, "dailyLimitL", s.dailyLimitL, 0, 1000000);
  take<uint8_t>(o, "tankMinPct", s.tankMinPct, 0, 90);
  take<float>(o, "tankEmptyCm", s.tankEmptyCm, 5, 450);
  take<float>(o, "tankFullCm", s.tankFullCm, 2, 400);
  take<float>(o, "tankCapacityL", s.tankCapacityL, 1, 1000000);
  take(o, "weatherEnabled", s.weatherEnabled, false, true);
  take<uint8_t>(o, "rainProbPct", s.rainProbPct, 10, 100);
  take<float>(o, "rainMm", s.rainMm, 0, 100);
  take<uint8_t>(o, "lookaheadH", s.lookaheadH, 1, 12);
  take<uint8_t>(o, "maxWaitH", s.maxWaitH, 1, 48);
  take<uint16_t>(o, "rainLockoutMin", s.rainLockoutMin, 0, 1440);
  take<uint16_t>(o, "weatherIntervalMin", s.weatherIntervalMin, 10, 360);
  take(o, "windowEnabled", s.windowEnabled, false, true);
  take<uint8_t>(o, "w1Start", s.w1Start, 0, 24);
  take<uint8_t>(o, "w1End", s.w1End, 0, 24);
  take<uint8_t>(o, "w2Start", s.w2Start, 0, 24);
  take<uint8_t>(o, "w2End", s.w2End, 0, 24);
  take(o, "heatBoost", s.heatBoost, false, true);
  take<float>(o, "heatTempC", s.heatTempC, 20, 60);
  take<uint8_t>(o, "heatBoostPct", s.heatBoostPct, 0, 30);
  take<uint16_t>(o, "soilDryRaw", s.soilDryRaw, 0, 4095);
  take<uint16_t>(o, "soilWetRaw", s.soilWetRaw, 0, 4095);
  take<uint16_t>(o, "rainWetRaw", s.rainWetRaw, 0, 4095);
  take<float>(o, "pumpLpm", s.pumpLpm, 0.1f, 2000);
  take<uint8_t>(o, "dryRunMin", s.dryRunMin, 0, 60);
  take<uint8_t>(o, "leakPct", s.leakPct, 0, 50);
  take(o, "relayActiveLow", s.relayActiveLow, false, true);

  if (s.moistStop <= s.moistStart) s.moistStop = min(100, s.moistStart + 5);
  return wifiChanged;
}
