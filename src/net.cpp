// WiFi: joins the farm WiFi; if that is not configured or unreachable it opens
// its own hotspot "HybridFlow-XXXX" with a captive portal so the farm can
// always be controlled from a phone, even with no router / internet.
#include "net.h"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include "config.h"
#include "settings.h"
#include "state.h"

static DNSServer dns;
static bool apOn = false;
static char apName[24];
static uint32_t lastTryMs = 0, connectedSinceMs = 0, disconnectedSinceMs = 0;
static bool wasConnected = false;

static void startAp() {
  if (apOn) return;
  WiFi.mode(settings.wifiSsid[0] ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(apName, AP_PASSWORD);
  dns.start(53, "*", WiFi.softAPIP());
  apOn = true;
  eventLog(EV_INFO, "Hotspot %s started — open http://%s", apName, WiFi.softAPIP().toString().c_str());
}

static void stopAp() {
  if (!apOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  eventLog(EV_INFO, "Hotspot stopped (farm WiFi connected)");
}

void netBegin() {
  snprintf(apName, sizeof(apName), "HybridFlow-%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
  WiFi.persistent(false);
  WiFi.setHostname(MDNS_NAME);
  configTime(settings.tzOffsetSec, 0, "pool.ntp.org", "time.google.com");

  if (settings.wifiSsid[0]) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(settings.wifiSsid, settings.wifiPass);
    Serial.printf("Connecting to %s", settings.wifiSsid);
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500);
      Serial.print('.');
    }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    wasConnected = true;
    connectedSinceMs = millis();
    eventLog(EV_INFO, "WiFi connected: %s (%s)", settings.wifiSsid, WiFi.localIP().toString().c_str());
  } else {
    startAp();
  }
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
}

void netLoop() {
  if (apOn) dns.processNextRequest();
  uint32_t now = millis();
  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected && !wasConnected) {
    wasConnected = true;
    connectedSinceMs = now;
    eventLog(EV_INFO, "WiFi connected (%s)", WiFi.localIP().toString().c_str());
  } else if (!connected && wasConnected) {
    wasConnected = false;
    disconnectedSinceMs = now;
    eventLog(EV_WARN, "WiFi lost — irrigation keeps running offline");
  }

  if (!settings.wifiSsid[0]) return;

  if (!connected) {
    // keep retrying the farm WiFi; open the hotspot if it stays down for 2 min
    if (now - lastTryMs > 30000) {
      lastTryMs = now;
      WiFi.begin(settings.wifiSsid, settings.wifiPass);
    }
    if (!apOn && now - disconnectedSinceMs > 120000) startAp();
  } else if (apOn && now - connectedSinceMs > 300000) {
    // give someone configuring via the hotspot 5 min before closing it
    stopAp();
  }
}

bool netApActive() { return apOn; }

void netToJson(JsonObject o) {
  bool c = WiFi.status() == WL_CONNECTED;
  o["connected"] = c;
  o["ssid"] = settings.wifiSsid;
  if (c) {
    o["ip"] = WiFi.localIP().toString();
    o["rssi"] = WiFi.RSSI();
  }
  o["ap"] = apOn;
  if (apOn) {
    o["apName"] = apName;
    o["apIp"] = WiFi.softAPIP().toString();
  }
  o["host"] = MDNS_NAME ".local";
}
