# Hybrid-Flow: smart farm irrigation (ESP32)

Hybrid-Flow is a weather-aware irrigation controller. It uses **soil moisture**, a **rain-drop sensor in the field**, the **weather forecast**, and **tank level** to decide when to water. It also serves a web portal where you can watch and control the whole farm from your phone.

The forecast saves water and power. The field sensor overrides the forecast whenever the forecast is wrong. All decisions are made on the ESP32 itself, so the farm keeps being watered correctly even without WiFi or internet.

---

## 1. Features

### What you asked for
| Requirement | How it works |
|---|---|
| Soil moisture | Resistive probe (FC-28 / YL-69 fork + LM393 module), with 16-sample averaging. You calibrate it in your own field soil from the portal. |
| Rain at the field | A rain-drop module (AO and DO both used, debounced). If rain is detected, irrigation stops immediately, including in the middle of a cycle. |
| Temperature and humidity | DHT11. These readings drive the heat-stress boost and the speed-of-sound correction for the tank sensor. |
| Tank level % | HC-SR04, median of 5 echoes plus smoothing. Shown as % and litres, with an estimate of how many days of water are left. Also used for pump protection and leak detection (see below). |
| Litres used | No flow sensor needed. Litres = pump rate (L/min) × running time, checked against the tank level drop after every run. Shown per run, per day and lifetime. |
| Web portal | A dashboard with live updates (WebSocket, every 2 s) and Control, Weather, History, Log and Settings tabs. Works on phone and desktop, in light and dark themes. |
| Weather API | [Open-Meteo](https://open-meteo.com): free, no API key. Looks ahead 12 hours by the hour and 3 days by the day. |
| Wait for rain | If the forecast says rain ≥ X % and ≥ Y mm within N hours, watering is held to save water and power. |
| Forecast is wrong | The rain sensor is the final authority. If the forecast rain doesn't come within the "max wait" time, the controller waters anyway. |
| Manual control | Run by **time** or by **litres**, stop at any time, switch AUTO/MANUAL, or pause automation for 2 h, 6 h, 24 h or 3 days. |

### Advanced features added
1. **Forecast trust learning.** Each time the controller waits for forecast rain, the field sensor scores whether the rain actually came (hits and misses). If forecasts for *your* field are often wrong (< 40 % hits), it automatically requires a higher rain probability before waiting. If they are reliable (> 80 %), it trusts them sooner.
2. **Water-saved accounting.** When a rain skip ends with rain wetting the soil, the controller adds the learned average litres per session to "Saved by rain".
3. **Cycle & soak (pulse irrigation).** For example: run 8 min, soak 10 min, repeat until the target moisture is reached. This gives less runoff and deeper roots, especially on clay or slopes.
4. **Watering windows.** Waters only in the morning or evening by default, to reduce evaporation.
5. **Critical-moisture override.** If the soil drops below the critical level, the crop is protected first: the controller ignores the forecast and the watering window.
6. **Heat-stress boost.** Above a set temperature (35 °C by default), the start/stop thresholds are raised so plants get more water.
7. **After-rain lockout.** After the rain sensor dries, irrigation waits a set time (90 min by default) to let the soil absorb the rain.
8. **Pump dry-run protection (using the tank sensor).** If the pump has run for 4 min but the tank level hasn't dropped as expected, the pump is dry, blocked, or a pipe is broken. The pump stops and locks out until you acknowledge the alarm.
9. **Leak detection (using the tank sensor).** If the tank drops 5 % or more in 30 min while the pump is off, an alarm is raised (leak, or a tap or valve left open).
10. **Tank protection.** The controller won't start below the minimum tank level, and always stops at 3 % or below.
11. **Daily water budget** (optional) and a **max run time** per session.
12. **Anti short-cycling.** The pump rests at least 15 s between starts. There is also a hard 180-minute cap on any single run.
13. **Sensor fault detection.** A dead soil module or a loose wire disables auto watering for safety. If a full watering session doesn't raise the soil reading (probe pulled out of the soil, or wire cut), auto watering locks until you acknowledge. DHT and tank faults are reported as alarms.
14. **Simple sensor power.** The soil and rain modules are powered continuously from 3V3 and read every 2 s. Resistive forks corrode faster when always powered, so check them every few months.
15. **"Why?" panel.** Shows each condition the controller is checking right now.
16. **History.** 24 h charts (soil with rain shading, tank, temperature/humidity) and 14 days of water use.
17. **Event log.** Every decision, alarm and pump start/stop, with timestamps.
18. **Offline-first networking.** If farm WiFi is down, the ESP32 opens its own hotspot with a captive portal.
19. **Timezone learned automatically** from the farm's GPS location, through the weather API.
20. **Firmware updates over WiFi** from the Settings tab (OTA). The portal is embedded in the firmware, so one `.bin` file updates both.
21. **Password protection** for every control action. Viewing is open.

---

## 2. Parts list

| Part | Notes |
|---|---|
| ESP32 DevKit (ESP32-WROOM-32) | |
| Resistive soil moisture sensor (FC-28 / YL-69 with LM393 module) | Expect to replace the fork every season or two. Stainless-steel or nickel-plated forks last longer. |
| Rain-drop sensor module (FC-37 / YL-83) | |
| DHT11 | Use the 3-pin module version (has a pull-up resistor) |
| HC-SR04 ultrasonic | For outdoor tanks, the waterproof **JSN-SR04T** works with the same code |
| Small DC pump | Switched by the relay |
| 1-channel 5 V relay module | LOW- or HIGH-trigger; the type is selectable in the portal |
| Resistors: 1 kΩ + 2 kΩ | Voltage divider for the HC-SR04 echo (5 V) |
| 5 V 2 A power supply | |

---

## 3. Wiring

All signals use **one side** of the ESP32 DevKit (the side with D34, D35, D32, D33, D25, D26, D27, D14, D12, D13), so it fits a small breadboard. The pins are set in `src/config.h`:

```
   ESP32 DevKit — one side
   ┌─────┐
   │ D34 ├──── Soil module AO           (analog, input-only pin)
   │ D35 ├──── Rain module AO           (analog, input-only pin)
   │ D32 ├──── Rain module DO
   │ D33 ├──[1k]──┬── HC-SR04 ECHO      (5 V signal → divider)
   │     │      [2k]
   │     │       GND
   │ D25 ├──── HC-SR04 TRIG
   │ D26 │     spare
   │ D27 ├──── DHT11 DATA
   │ D14 │     spare
   │ D12 │     leave unconnected (boot pin)
   │ D13 ├──── Relay module IN
   │ GND ├──── all grounds
   │ VIN ├──── HC-SR04 VCC (5 V)
   └─────┘
   3V3 (other side, or the breadboard rail) → soil VCC, rain VCC, DHT11 VCC, relay module VCC
```

| Signal | ESP32 pin | Why this pin |
|---|---|---|
| Soil module AO | D34 | Analog on ADC1 (works with WiFi on); input-only pin is fine for a sensor |
| Rain module AO | D35 | Analog on ADC1; input-only |
| Rain module DO | D32 | Digital input |
| HC-SR04 ECHO | D33 | Input. **ECHO is 5 V: use the 1 kΩ / 2 kΩ divider** |
| HC-SR04 TRIG | D25 | Needs an output pin |
| Relay module IN | D13 | Output. Switches the pump through the relay. Clean at boot, so the relay doesn't click on power-up |
| DHT11 DATA | D27 | Digital in/out |
| — | D12 | **Leave unconnected.** It must be LOW when the ESP32 starts, or the board may not boot |
| — | D14, D26 | Spare |
| Soil + rain + DHT11 VCC | 3V3 | Never use 5 V for the soil or rain modules: their AO would exceed the ESP32's 3.3 V limit |

Only GND and VIN are on this side; 3V3 is on the other side of the board. If you can't reach it, run one jumper from the 3V3 pin to a breadboard power rail. The pins don't need to be on your side for that.

### Pump through the relay
D13 drives the relay module's **IN** pin, and the relay's contacts switch the pump's power. The pump never draws current from the ESP32 pins.

```
 Relay module                     Pump circuit
 ┌───────────┐
 │ VCC ──────┼── ESP32 3V3
 │ GND ──────┼── ESP32 GND
 │ IN  ──────┼── ESP32 D13
 │           │
 │ COM ──────┼── Pump supply (+)   (3V3 rail, 5 V, or a separate battery/adapter)
 │ NO  ──────┼── Pump (+)
 │ NC        │   (unused)
 └───────────┘   Pump (−) ── supply (−) / GND
```

- **COM and NO:** use NO (normally open), so the pump stays off if the ESP32 resets or loses power.
- **Relay type:** most relay modules are **LOW-trigger**. That's the default in *Settings → Pump & protection → "Relay switches ON with a LOW signal"*. If the pump runs when the portal says OFF, untick that box and save; it takes effect immediately.
- **Relay power: use 3V3, not VIN.** A LOW-trigger relay module powered from 5 V can't be switched off by the ESP32's 3.3 V HIGH, so it stays on all the time. Powered from 3V3, HIGH (3.3 V) switches it off and LOW switches it on. Tested and working with this build.
- **If your relay clicks weakly on 3V3:** keep its VCC on VIN (5 V) and add an NPN transistor (2N2222 / BC547): D13 → 1 kΩ → base, emitter → GND, collector → relay IN. Then untick "Relay switches ON with a LOW signal".
- **Pump power:** a small DC pump can be powered from the 3V3 rail through the relay, like you did before. A bigger pump should have its own supply; connect only its switching wire through COM/NO.
- **Larger pumps:** for mains (230 V) pumps, use a relay or contactor rated for the motor plus an RCD/ELCB, and have the mains side wired by an electrician.

Keep the ESP32 in a waterproof box. To change any pin, edit `src/config.h`.

---

## 4. Build and flash

You need [PlatformIO](https://platformio.org) (CLI, or the VS Code extension).

```bash
pio run -t upload
```

```bash
pio device monitor
```

You don't need a separate filesystem upload. The web files in `web/` are gzipped and compiled into the firmware automatically.

### First-time setup
1. After flashing, the ESP32 opens a hotspot named **`HybridFlow-XXXX`** with password **`hybridflow`**.
2. Connect your phone. The portal opens automatically; if it doesn't, go to **http://192.168.4.1**.
3. Go to **Settings** and:
   - enter your farm WiFi name and password;
   - tap **"Use my phone's location"** (needed for the weather forecast);
   - set a new admin password. The default is **`admin123`**, so change it.
4. Tap **Save**. The controller restarts and joins your WiFi. Open **http://hybridflow.local** or the IP address shown in the serial monitor.

### Calibrate (about 5 minutes, in Settings)
- **Soil (resistive):** push the fork fully into the soil at root depth, about 10 cm from a dripper. When the soil is at its driest, tap *Probe in dry soil now*. About 30 minutes after a thorough watering, tap *Probe in wet soil now*. Don't calibrate in air or in a glass of water: those readings are far from real soil, and you would get misleading %. A higher raw value means drier soil.
- **Tank:** when the tank is empty, tap *Tank is empty now*. When it is full, tap *Tank is full now*. Or type both distances in by hand.
- **Rain:** look at the live "rain raw" value while the board is dry and then with a few drops on it. Set "Rain if AO below" between the two values.
- **Pump rate:** run the pump into a bucket for 1 minute, measure the litres, and enter the result as *Pump flow rate (L/min)*. Or, after a run of at least 3 minutes, read the Log line "Tank dropped … L → pump ≈ … L/min".

---

## 5. How the controller decides (AUTO mode)

The checks run every second, in this order:

```
Dry-run alarm latched?          → stop, wait for acknowledge
Paused by user?                 → wait
Soil sensor faulty?             → no auto watering (fail-safe)
Soaking between cycles?         → wait
Soil moisture ≥ start level?    → nothing to do  (records "saved by rain" if rain did the job)
Raining at the field?           → hold (rain sensor always wins)
Rained recently?                → wait for the soil to absorb it   (skipped if CRITICAL)
Tank below minimum?             → hold + alarm
Daily water budget used?        → hold
Forecast: rain ≥ p% & ≥ mm soon → WAIT, save water & power          (skipped if CRITICAL)
   └─ gives up after "max wait" hours if the rain never comes
Outside watering window?        → wait for the window               (skipped if CRITICAL)
→ START PUMP (cycle & soak until moisture ≥ stop level)
```

While the pump runs, it stops on any of these: target moisture reached, rain detected, tank level not dropping (dry run), tank empty, max run time reached, or the daily budget used up.

> The dry-run and leak checks assume the pump draws from the monitored tank. If the tank is being refilled while you irrigate, set *Dry-run check* to 0 in Settings.

Manual runs ignore the forecast and the rain sensor (it's your decision). The pump protections still apply: dry run, tank empty, and the 180-minute hard cap.

---

## 6. Try the portal without hardware

```bash
python3 -m http.server 8765 --directory web
```

Then open http://localhost:8765/?demo. This shows a simulated farm, so you can try every screen.

## 7. API (for apps or Home Assistant)

| Method | Path | Body |
|---|---|---|
| GET | `/api/status` | — |
| GET | `/api/history` | — |
| GET | `/api/events` | — |
| GET/POST | `/api/settings` | JSON with any setting keys |
| POST | `/api/control` | `{"cmd":"pump_on","minutes":10}` · `{"cmd":"pump_on","litres":50}` · `pump_off` · `{"cmd":"mode","mode":"auto"}` · `{"cmd":"pause","hours":6}` · `resume` · `ack` · `weather_refresh` · `calib_soil_dry` · `calib_soil_wet` · `calib_tank_empty` · `calib_tank_full` · `reset_stats` · `reboot` |
| WS | `/ws` | Live status JSON every 2 s |

POST requests need HTTP Basic auth with user `admin` and your admin password.

## 8. Project layout
```
platformio.ini         build config
scripts/embed_web.py   gzips web/ into src/web_assets.h at build time
src/config.h           pins, timings, hard safety limits
src/sensors.*          soil, rain, DHT11, HC-SR04
src/irrigation.*       decision engine + safety
src/weather.*          Open-Meteo client (own FreeRTOS task)
src/state.*            lock, event log, statistics, history
src/settings.*         settings stored in flash (NVS)
src/net.*              WiFi, hotspot fallback, captive portal, mDNS
src/web.*              web server, REST API, WebSocket, OTA
web/                   the portal (HTML/CSS/JS, no external libraries)
```
