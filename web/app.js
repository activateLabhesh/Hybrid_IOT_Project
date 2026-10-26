/* Hybrid-Flow portal. Talks to the ESP32 over REST + WebSocket.
   Open web/index.html directly from disk (or add ?demo) to run a simulated farm. */
'use strict';

const $ = (s) => document.querySelector(s);
const $$ = (s) => Array.from(document.querySelectorAll(s));
const DEMO = location.protocol === 'file:' || /[?&]demo/.test(location.search);

let S = null;          // latest status from controller
let CFG = null;        // settings
let lastMsgAt = 0;
let runBy = 'time';
let auth = localStorage.getItem('hf_auth') || sessionStorage.getItem('hf_auth') || '';

// ------------------------------------------------------------------ helpers
const fmt = (v, d = 0) => (v === undefined || v === null || Number.isNaN(v)) ? '--' : Number(v).toFixed(d);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const pad = (n) => String(n).padStart(2, '0');

function farmDate(epoch) {
  // Show farm-local time (timezone learned from the weather API), whatever the phone's timezone.
  const tz = S ? S.tz || 0 : 0;
  return new Date((epoch + tz) * 1000);
}
function fmtClock(epoch) {
  if (!epoch) return '--:--';
  const d = farmDate(epoch);
  return pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes());
}
function fmtDay(epoch) {
  const d = farmDate(epoch);
  return d.toLocaleDateString(undefined, { day: 'numeric', month: 'short', timeZone: 'UTC' });
}
function fmtDur(sec) {
  sec = Math.max(0, Math.round(sec));
  const m = Math.floor(sec / 60), s = sec % 60;
  return m >= 60 ? `${Math.floor(m / 60)}h ${m % 60}m` : `${m}m ${pad(s)}s`;
}
function toast(msg, bad) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'toast' + (bad ? ' bad' : '');
  clearTimeout(toast._t);
  toast._t = setTimeout(() => t.classList.add('hidden'), 3200);
}

const WMO = (c) => {
  if (c === 0) return ['☀️', 'Clear'];
  if (c <= 2) return ['🌤️', 'Partly cloudy'];
  if (c === 3) return ['☁️', 'Overcast'];
  if (c === 45 || c === 48) return ['🌫️', 'Fog'];
  if (c >= 51 && c <= 57) return ['🌦️', 'Drizzle'];
  if (c >= 61 && c <= 67) return ['🌧️', 'Rain'];
  if (c >= 71 && c <= 77) return ['❄️', 'Snow'];
  if (c >= 80 && c <= 82) return ['🌦️', 'Showers'];
  if (c >= 95) return ['⛈️', 'Thunderstorm'];
  return ['🌡️', '—'];
};

// ------------------------------------------------------------------ API
async function api(path, body, needAuth) {
  if (DEMO) return Demo.api(path, body);
  const opts = { method: body !== undefined || needAuth ? 'POST' : 'GET', headers: {} };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  if (needAuth) {
    if (!auth) await login();
    opts.headers.Authorization = 'Basic ' + auth;
  }
  let r = await fetch(path, opts);
  if (r.status === 401 && needAuth) {
    auth = '';
    localStorage.removeItem('hf_auth');
    sessionStorage.removeItem('hf_auth');
    await login('Wrong password, try again');
    opts.headers.Authorization = 'Basic ' + auth;
    r = await fetch(path, opts);
  }
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.error || 'HTTP ' + r.status);
  return data;
}
const control = (cmd, extra) => api('/api/control', Object.assign({ cmd }, extra || {}), true);

async function act(fn, okMsg) {
  try {
    await fn();
    if (okMsg) toast(okMsg);
    setTimeout(refreshStatus, 300);
  } catch (e) {
    if (e.message !== 'cancelled') toast(e.message, true);
  }
}

// Login modal: resolves once a password is verified by the controller
function login(msg) {
  return new Promise((resolve, reject) => {
    const m = $('#loginModal');
    $('#loginErr').textContent = msg || '';
    $('#loginPass').value = '';
    m.classList.remove('hidden');
    setTimeout(() => $('#loginPass').focus(), 50);
    $('#loginForm').onsubmit = async (e) => {
      e.preventDefault();
      const token = btoa('admin:' + $('#loginPass').value);
      const r = await fetch('/api/login', { method: 'POST', headers: { Authorization: 'Basic ' + token } }).catch(() => null);
      if (!r || !r.ok) { $('#loginErr').textContent = r ? 'Wrong password' : 'Controller not reachable'; return; }
      auth = token;
      ($('#loginRemember').checked ? localStorage : sessionStorage).setItem('hf_auth', token);
      m.classList.add('hidden');
      resolve();
    };
    $('#loginCancel').onclick = () => { m.classList.add('hidden'); reject(new Error('cancelled')); };
  });
}

// ------------------------------------------------------------------ live data
let ws = null;
function connectWs() {
  if (DEMO) return;
  try { ws = new WebSocket(`ws://${location.host}/ws`); } catch (e) { return; }
  ws.onmessage = (ev) => { try { onStatus(JSON.parse(ev.data)); } catch (e) { /* ignore */ } };
  ws.onclose = () => { ws = null; setTimeout(connectWs, 3000); };
}
async function refreshStatus() {
  try { onStatus(await api('/api/status')); } catch (e) { /* offline */ }
}
setInterval(() => {
  // polling fallback when the websocket is down
  if (DEMO || !ws || ws.readyState !== 1 || Date.now() - lastMsgAt > 6000) refreshStatus();
  const online = Date.now() - lastMsgAt < 8000;
  $('#connDot').className = 'dot ' + (online ? 'on' : 'off');
  $('#connText').textContent = online ? (DEMO ? 'Demo' : 'Online') : 'Offline';
}, 2500);

function onStatus(s) {
  S = s;
  lastMsgAt = Date.now();
  render();
}

// ------------------------------------------------------------------ render
const ST_LABEL = ['All good', 'Watering', 'Waiting', 'On hold', 'Attention'];

function render() {
  if (!S) return;
  const sn = S.sensors, irr = S.irr, st = S.stats, wx = S.weather;

  $('#farmName').textContent = S.farm || 'Hybrid-Flow';
  document.title = (irr.pump ? '💧 ' : '') + (S.farm || 'Hybrid-Flow');
  $('#clock').textContent = fmtClock(S.time);
  const pill = $('#modePill');
  pill.textContent = irr.mode === 'auto' ? 'AUTO' : 'MANUAL';
  pill.classList.toggle('manual', irr.mode !== 'auto');

  // Status banner
  const box = $('#statusBox');
  box.className = 'status st' + irr.statusCode + (irr.pump ? ' watering' : '');
  $('#statusLabel').textContent = ST_LABEL[irr.statusCode] || 'Status';
  $('#statusText').textContent = irr.status;

  const al = irr.alarms || [];
  $('#alarmBox').classList.toggle('hidden', !al.length);
  $('#alarmList').innerHTML = al.map((a) => `<li>${esc(a)}</li>`).join('');
  $('#ackBtn').classList.toggle('hidden', !(irr.alarmBits & (1 | 2 | 128)));  // latched alarms

  // Soil gauge
  const C = 2 * Math.PI * 50;
  const arc = $('#soilArc');
  const m = sn.soilOk ? sn.moisture : 0;
  arc.style.strokeDasharray = C;
  arc.style.strokeDashoffset = C * (1 - m / 100);
  arc.style.stroke = m < irr.effStart ? 'var(--warn)' : m >= irr.effStop ? 'var(--water)' : 'var(--good)';
  $('#markStart').style.transform = `rotate(${irr.effStart * 3.6}deg)`;
  $('#markStop').style.transform = `rotate(${irr.effStop * 3.6}deg)`;
  $('#soilVal').textContent = sn.soilOk ? fmt(sn.moisture) : '!!';
  $('#soilFoot').innerHTML = sn.soilOk
    ? `Start below <b>${fmt(irr.effStart)}%</b> · stop at <b>${fmt(irr.effStop)}%</b>${irr.heatBoost ? ' · 🔥 heat boost' : ''}`
    : '<span class="warn">Sensor fault — check wiring</span>';

  // Tank
  $('#tankFill').style.height = (sn.tankOk ? sn.tankPct : 0) + '%';
  $('#tankMin').style.bottom = (CFG ? CFG.tankMinPct : 10) + '%';
  $('#tankVal').textContent = sn.tankOk ? fmt(sn.tankPct) : '!!';
  $('#tankL').textContent = sn.tankOk ? `${fmt(sn.tankL)} of ${fmt(sn.tankCap)} L` : 'Sensor not reading';
  const daysLeft = st.yesterday > 1 && sn.tankOk ? sn.tankL / st.yesterday : null;
  $('#tankFoot').textContent = daysLeft !== null ? `≈ ${fmt(daysLeft, 1)} days at yesterday's use` : (sn.tankOk ? `Water level ${fmt(sn.distance)} cm below sensor` : '');

  // Rain sensor
  $('#rainIcon').textContent = sn.raining ? '🌧️' : '🌤️';
  $('#rainIcon').classList.toggle('on', sn.raining);
  $('#rainVal').textContent = sn.raining ? 'Raining' : 'Dry';
  $('#rainSub').textContent = sn.raining ? `Intensity ${sn.rainIntensity}%` : sn.rainAgoMin >= 0 ? `Last rain ${fmtAgo(sn.rainAgoMin)}` : 'No rain since start';
  $('#rainFoot').textContent = irr.forecast.rain ? `Forecast: ${irr.forecast.prob}% chance, ${fmt(irr.forecast.mm, 1)} mm soon` : (wx.valid ? 'No significant rain forecast' : 'Forecast unavailable');

  // Air
  $('#tempVal').textContent = sn.dhtOk ? fmt(sn.temp, 1) : '!!';
  $('#humVal').textContent = sn.dhtOk ? fmt(sn.hum) : '!!';
  $('#airFoot').textContent = sn.dhtOk ? airHint(sn.temp, sn.hum) : 'DHT11 not responding';

  // Pump
  $('#pumpBig').textContent = irr.pump ? 'ON' : 'OFF';
  $('#pumpBig').className = 'big' + (irr.pump ? ' good' : '');
  $('#pumpState').textContent = irr.pump ? `${irr.source} · ~${fmt(sn.flow, 1)} L/min` : 'Idle';
  $('#runL').textContent = irr.pump ? fmt(irr.runLitres, 1) : '0';
  $('#runTime').textContent = irr.pump ? fmtDur(irr.runSec) : 'this run';
  const qp = $('#quickPump');
  qp.textContent = irr.pump ? 'Stop pump' : 'Start pump (10 min)';
  qp.className = 'btn wide ' + (irr.pump ? 'danger' : 'primary');

  // Usage
  $('#todayL').textContent = fmt(st.today);
  $('#savedL').textContent = fmt(st.saved);
  $('#usageFoot').textContent = `Yesterday ${fmt(st.yesterday)} L · ${st.skips} rain skip${st.skips === 1 ? '' : 's'}`;

  // Forecast bars
  renderBars($('#dashBars'), wx, 70);
  $('#fcSummary').textContent = wx.valid ? `updated ${wx.ageMin} min ago` : (wx.error || 'not available');

  renderControl();
  renderWeather();
  renderLive();
}

function fmtAgo(min) {
  if (min < 1) return 'just now';
  if (min < 60) return `${min} min ago`;
  if (min < 1440) return `${Math.floor(min / 60)} h ago`;
  return `${Math.floor(min / 1440)} days ago`;
}
function airHint(t, h) {
  if (t >= 38) return 'Extreme heat — crops lose water fast';
  if (t >= 32 && h < 40) return 'Hot & dry — high evaporation';
  if (h > 85) return 'Very humid — watch for fungal disease';
  return 'Comfortable growing conditions';
}

function renderBars(el, wx, h) {
  if (!wx.valid) { el.innerHTML = '<div class="muted">Forecast not available yet.</div>'; return; }
  const hour0 = farmDate(wx.fetched || S.time).getUTCHours();
  const maxMm = Math.max(1, ...wx.hMm);
  el.innerHTML = wx.hProb.map((p, i) => {
    const tip = `${pad((hour0 + i) % 24)}:00 — ${p}% chance, ${wx.hMm[i]} mm`;
    const label = h > 80 ? `${p}%<br>${pad((hour0 + i) % 24)}h` : (i % 3 === 0 ? pad((hour0 + i) % 24) + 'h' : '&nbsp;');
    const height = Math.max(p, (wx.hMm[i] / maxMm) * 100 * 0.5);
    return `<div class="bar" title="${tip}"><i class="${p >= (S.irr.forecast.needProb || 60) ? 'hi' : ''}" style="height:${height}%"></i><span>${label}</span></div>`;
  }).join('');
}

function renderControl() {
  const irr = S.irr, sn = S.sensors;
  $$('#modeSeg button').forEach((b) => b.classList.toggle('active', b.dataset.mode === irr.mode));
  $('#stopBtn').disabled = !irr.pump;
  $('#runInfo').textContent = irr.pump
    ? `Running ${fmtDur(irr.runSec)} · ~${fmt(irr.runLitres, 1)} L (estimated from pump rate)`
    : (sn.raining ? 'Note: it is raining in the field right now.' : '');
  $('#pauseInfo').textContent = irr.pausedMin > 0 ? `Paused — resumes in ${fmtAgo(irr.pausedMin).replace(' ago', '')}` : 'Automation active.';
  $('#resumeBtn').disabled = !(irr.pausedMin > 0);

  if (!CFG) return;
  const hour = S.time ? farmDate(S.time).getUTCHours() : -1;
  const inWin = (s, e) => s === e ? false : s < e ? hour >= s && hour < e : hour >= s || hour < e;
  const winOk = !CFG.windowEnabled || hour < 0 || inWin(CFG.w1Start, CFG.w1End) || inWin(CFG.w2Start, CFG.w2End);
  const lockLeft = sn.rainAgoMin >= 0 ? CFG.rainLockoutMin - sn.rainAgoMin : -1;
  const rows = [
    [irr.mode === 'auto' ? '✅' : '✋', 'Mode', irr.mode === 'auto' ? 'Automatic' : 'Manual only — controller will not start the pump'],
    [!sn.soilOk ? '⚠️' : sn.moisture < irr.effStart ? '💧' : '✅', 'Soil moisture',
      sn.soilOk ? `${fmt(sn.moisture)}% (start < ${fmt(irr.effStart)}%, stop ≥ ${fmt(irr.effStop)}%, critical < ${CFG.moistCritical}%)${irr.heatBoost ? ' — raised by heat boost' : ''}` : 'Sensor fault'],
    [sn.raining ? '🌧️' : '✅', 'Rain sensor', sn.raining ? 'Raining — irrigation blocked' : 'Dry'],
    [lockLeft > 0 ? '⏳' : '✅', 'After-rain wait', lockLeft > 0 ? `${lockLeft} min left of ${CFG.rainLockoutMin} min` : 'Not active'],
    [irr.forecast.rain ? '⏳' : '✅', 'Forecast', !S.weather.valid ? 'Unavailable — rain sensor only' :
      `${irr.forecast.prob}% max chance / ${fmt(irr.forecast.mm, 1)} mm in next ${CFG.lookaheadH} h — waits if ≥ ${irr.forecast.needProb}% and ≥ ${CFG.rainMm} mm`],
    [sn.tankOk && sn.tankPct < CFG.tankMinPct ? '⛔' : '✅', 'Tank', sn.tankOk ? `${fmt(sn.tankPct)}% (minimum ${CFG.tankMinPct}%)` : 'Sensor not reading — dry-run protection still active'],
    [winOk ? '✅' : '⏳', 'Watering window', CFG.windowEnabled ? `${pad(CFG.w1Start)}–${pad(CFG.w1End)}h and ${pad(CFG.w2Start)}–${pad(CFG.w2End)}h, now ${hour >= 0 ? pad(hour) + 'h' : 'unknown'}` : 'Any time'],
    [CFG.dailyLimitL > 0 && S.stats.today >= CFG.dailyLimitL ? '⛔' : '✅', 'Daily budget', CFG.dailyLimitL > 0 ? `${fmt(S.stats.today)} / ${CFG.dailyLimitL} L` : 'No limit'],
  ];
  $('#whyTable').innerHTML = rows.map((r) => `<tr><td>${r[0]}</td><td>${r[1]}</td><td>${esc(r[2])}</td></tr>`).join('');
}

function renderWeather() {
  const wx = S.weather, st = S.stats;
  $('#fcHits').textContent = st.hits;
  $('#fcMiss').textContent = st.misses;
  $('#fcNeed').textContent = S.irr.forecast.needProb + '%';
  if (!wx.valid) {
    $('#wxNow').innerHTML = `<div class="muted">No forecast yet. ${esc(wx.error || 'Connect the controller to the internet and set the farm location.')}</div>`;
    $('#wxBars').innerHTML = '';
    $('#wxDays').innerHTML = '';
    $('#wxAge').textContent = '';
    return;
  }
  const [e, t] = WMO(wx.code);
  $('#wxAge').textContent = `updated ${wx.ageMin} min ago`;
  $('#wxNow').innerHTML = `<div class="wx-emoji">${e}</div>
    <div><div class="big">${fmt(wx.temp, 1)}<small>°C</small></div><div class="muted">${t}</div></div>
    <div><div class="big">${fmt(wx.hum)}<small>%</small></div><div class="muted">Humidity</div></div>
    <div><div class="big">${fmt(wx.wind)}<small>km/h</small></div><div class="muted">Wind</div></div>
    <div><div class="big">${fmt(wx.days[0].et0, 1)}<small>mm</small></div><div class="muted">Crop water demand today (ET₀)</div></div>`;
  renderBars($('#wxBars'), wx, 130);
  const names = ['Today', 'Tomorrow', 'Day after'];
  $('#wxDays').innerHTML = wx.days.map((d, i) => {
    const [de, dt] = WMO(d.code);
    return `<div class="day"><div class="muted">${names[i]}</div><div class="e">${de}</div>
      <div><b>${fmt(d.max)}° / ${fmt(d.min)}°</b> · ${dt}</div>
      <div class="muted">Rain ${d.prob}% · ${fmt(d.rain, 1)} mm · ET₀ ${fmt(d.et0, 1)} mm</div></div>`;
  }).join('');
}

function renderLive() {
  const sn = S.sensors, n = S.net;
  $('#calDist').textContent = fmt(sn.distance, 1);
  $('#calSoil').textContent = sn.soilRaw;
  $('#calRain').textContent = sn.rainRaw;
  $('#netInfo').textContent = n.connected
    ? `Connected to ${n.ssid} · ${n.ip} · signal ${n.rssi} dBm · also at http://${n.host}`
    : n.ap ? `Not on farm WiFi. Hotspot "${n.apName}" active at ${n.apIp}` : 'Offline';
  $('#sysInfo').textContent = `Firmware ${S.fw} · up ${fmtDur(S.uptime)} · free memory ${Math.round(S.heap / 1024)} KB`;
  $('#lifeTotal').textContent = fmt(S.stats.total);
  $('#lifeSaved').textContent = fmt(S.stats.saved);
  $('#lifePump').textContent = fmt(S.stats.pumpHours, 1);
}

// ------------------------------------------------------------------ charts
function lineChart(el, t, series, opt) {
  const W = Math.max(280, el.clientWidth), H = el.clientHeight || 190;
  const L = 32, R = 8, T = 8, B = 20;
  const n = t.length;
  if (n < 2) { el.innerHTML = '<div class="muted">Collecting data — a point is recorded every 5 minutes.</div>'; return; }
  let lo = opt.min, hi = opt.max;
  if (lo === undefined || hi === undefined) {
    const vals = series.flatMap((s) => s.data).filter((v) => v !== null);
    lo = Math.floor(Math.min(...vals) - 2); hi = Math.ceil(Math.max(...vals) + 2);
  }
  const x = (i) => L + (i / (n - 1)) * (W - L - R);
  const y = (v) => T + (1 - (v - lo) / (hi - lo || 1)) * (H - T - B);
  let svg = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none">`;
  for (let k = 0; k <= 4; k++) {
    const v = lo + ((hi - lo) * k) / 4;
    svg += `<line class="axis" x1="${L}" x2="${W - R}" y1="${y(v)}" y2="${y(v)}"/><text x="${L - 4}" y="${y(v) + 3}" text-anchor="end">${Math.round(v)}</text>`;
  }
  // rain shading
  if (opt.rain) opt.rain.forEach((r, i) => {
    if (r && i < n - 1) svg += `<rect x="${x(i)}" y="${T}" width="${x(i + 1) - x(i)}" height="${H - T - B}" fill="var(--water-soft)" opacity=".5"/>`;
  });
  (opt.bands || []).forEach((b) => {
    svg += `<line x1="${L}" x2="${W - R}" y1="${y(b.y)}" y2="${y(b.y)}" stroke="${b.color}" stroke-dasharray="4 4"/>`;
  });
  series.forEach((s) => {
    let d = '', pen = false;
    s.data.forEach((v, i) => {
      if (v === null) { pen = false; return; }
      d += (pen ? 'L' : 'M') + x(i).toFixed(1) + ' ' + y(v).toFixed(1);
      pen = true;
    });
    svg += `<path d="${d}" fill="none" stroke="${s.color}" stroke-width="2" stroke-linejoin="round"/>`;
  });
  // time labels
  for (let k = 0; k < 5; k++) {
    const i = Math.round((k / 4) * (n - 1));
    const lbl = t[i] > 1e9 ? fmtClock(t[i]) : '';
    svg += `<text x="${x(i)}" y="${H - 4}" text-anchor="middle">${lbl}</text>`;
  }
  svg += '</svg>';
  const legend = series.map((s) => `<span><i style="background:${s.color}"></i>${s.label}</span>`)
    .concat((opt.bands || []).map((b) => `<span><i style="background:${b.color}"></i>${b.label}</span>`)).join('');
  el.innerHTML = svg + `<div class="legend">${legend}</div>`;
}

function barChart(el, labels, data, color) {
  const W = Math.max(280, el.clientWidth), H = el.clientHeight || 190;
  const L = 36, B = 20, T = 14;
  const max = Math.max(1, ...data);
  const bw = (W - L) / data.length;
  let svg = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none">`;
  for (let k = 0; k <= 4; k++) {
    const v = (max * k) / 4, yy = T + (1 - k / 4) * (H - T - B);
    svg += `<line class="axis" x1="${L}" x2="${W}" y1="${yy}" y2="${yy}"/><text x="${L - 4}" y="${yy + 3}" text-anchor="end">${Math.round(v)}</text>`;
  }
  data.forEach((v, i) => {
    const h = (v / max) * (H - T - B);
    svg += `<rect x="${L + i * bw + 3}" y="${H - B - h}" width="${bw - 6}" height="${h}" rx="3" fill="${color}"><title>${labels[i]}: ${fmt(v, 1)} L</title></rect>`;
    if (i % 2 === 1 || data.length < 8) svg += `<text x="${L + i * bw + bw / 2}" y="${H - 5}" text-anchor="middle">${labels[i]}</text>`;
  });
  el.innerHTML = svg + '</svg>';
}

async function loadHistory() {
  try {
    const h = await api('/api/history');
    const c = CFG || { moistStart: 35, moistStop: 60 };
    lineChart($('#chMoist'), h.t, [{ data: h.moist, color: 'var(--good)', label: 'Soil %' }],
      { min: 0, max: 100, rain: h.rain, bands: [{ y: c.moistStart, color: 'var(--warn)', label: 'Start' }, { y: c.moistStop, color: 'var(--water)', label: 'Stop' }] });
    lineChart($('#chTank'), h.t, [{ data: h.tank, color: 'var(--water)', label: 'Tank %' }], { min: 0, max: 100 });
    lineChart($('#chAir'), h.t, [{ data: h.temp, color: '#ef4444', label: '°C' }, { data: h.hum, color: '#8b5cf6', label: 'Humidity %' }], { min: 0, max: 100 });
    // daily[0] = yesterday ... daily[13] = 14 days ago; show oldest -> today
    const days = h.daily.slice().reverse().concat([S ? S.stats.today : 0]);
    const now = S && S.time ? S.time : Date.now() / 1000;
    const labels = days.map((_, i) => (i === days.length - 1 ? 'Today' : fmtDay(now - (days.length - 1 - i) * 86400)));
    barChart($('#chDaily'), labels, days, 'var(--water)');
  } catch (e) { /* offline */ }
}

async function loadLog() {
  try {
    const ev = await api('/api/events');
    $('#logList').innerHTML = ev.map((e) => {
      const when = e.t ? `${fmtDay(e.t)} ${fmtClock(e.t)}` : `boot +${fmtDur(e.up)}`;
      return `<li class="l${e.lvl}"><time>${when}</time><span>${esc(e.msg)}</span></li>`;
    }).join('') || '<li class="muted">No events yet.</li>';
  } catch (e) { /* offline */ }
}

// ------------------------------------------------------------------ settings
async function loadSettings() {
  try {
    CFG = await api('/api/settings');
    const f = $('#settingsForm');
    for (const [k, v] of Object.entries(CFG)) {
      const inp = f.elements[k];
      if (!inp) continue;
      if (inp.type === 'checkbox') inp.checked = !!v;
      else inp.value = typeof v === 'number' && !Number.isInteger(v) ? +v.toFixed(4) : v;
    }
    render();
  } catch (e) { setTimeout(loadSettings, 3000); }
}

$('#settingsForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const f = e.target, out = {};
  for (const inp of f.elements) {
    if (!inp.name) continue;
    if (inp.type === 'checkbox') out[inp.name] = inp.checked;
    else if (inp.type === 'number') { if (inp.value !== '') out[inp.name] = Number(inp.value); }
    else if (inp.name === 'adminPass') { if (inp.value) out.adminPass = inp.value; }
    else out[inp.name] = inp.value;
  }
  act(async () => {
    const r = await api('/api/settings', out, true);
    if (out.adminPass) {
      auth = btoa('admin:' + out.adminPass);
      localStorage.getItem('hf_auth') ? localStorage.setItem('hf_auth', auth) : sessionStorage.setItem('hf_auth', auth);
      f.elements.adminPass.value = '';
    }
    $('#saveMsg').textContent = r.reboot ? 'Saved — controller is restarting to join the new WiFi.' : 'Saved.';
    setTimeout(() => ($('#saveMsg').textContent = ''), 5000);
    loadSettings();
  }, 'Settings saved');
});

$('#geoBtn').onclick = () => {
  if (!navigator.geolocation) return toast('Location not available on this device', true);
  navigator.geolocation.getCurrentPosition((p) => {
    const f = $('#settingsForm');
    f.elements.lat.value = p.coords.latitude.toFixed(4);
    f.elements.lon.value = p.coords.longitude.toFixed(4);
    toast('Location filled — press Save');
  }, () => toast('Could not get location (needs permission; works best over https or on the phone itself)', true));
};

$('#otaBtn').onclick = () => act(async () => {
  const file = $('#otaFile').files[0];
  if (!file) throw new Error('Choose firmware.bin first');
  if (DEMO) throw new Error('Not available in demo mode');
  if (!auth) await login();
  const fd = new FormData();
  fd.append('firmware', file);
  toast('Uploading firmware… do not power off');
  const r = await fetch('/api/ota', { method: 'POST', body: fd, headers: { Authorization: 'Basic ' + auth } });
  if (!r.ok) throw new Error('Update failed (' + r.status + ')');
}, 'Firmware updated — controller is restarting');

// ------------------------------------------------------------------ controls
$$('#tabs button').forEach((b) => b.addEventListener('click', () => {
  $$('#tabs button').forEach((x) => x.classList.toggle('active', x === b));
  $$('.tab').forEach((t) => t.classList.toggle('active', t.id === 'tab-' + b.dataset.tab));
  if (b.dataset.tab === 'history') loadHistory();
  if (b.dataset.tab === 'log') loadLog();
  try { localStorage.setItem('hf_tab', b.dataset.tab); } catch (e) { /* ignore */ }
}));

$('#modePill').onclick = () => act(() => control('mode', { mode: S && S.irr.mode === 'auto' ? 'manual' : 'auto' }), 'Mode changed');
$$('#modeSeg button').forEach((b) => (b.onclick = () => act(() => control('mode', { mode: b.dataset.mode }), 'Mode changed')));
$('#ackBtn').onclick = () => act(() => control('ack'), 'Alarms acknowledged');
$('#quickPump').onclick = () => act(() => (S && S.irr.pump ? control('pump_off') : control('pump_on', { minutes: 10 })));
$('#stopBtn').onclick = () => act(() => control('pump_off'), 'Pump stopped');
$('#resumeBtn').onclick = () => act(() => control('resume'), 'Automation resumed');
$$('[data-pause]').forEach((b) => (b.onclick = () => act(() => control('pause', { hours: +b.dataset.pause }), `Paused for ${b.textContent}`)));
$('#wxRefresh').onclick = () => act(() => control('weather_refresh'), 'Forecast refresh requested');
$('#logRefresh').onclick = loadLog;
$$('[data-cmd]').forEach((b) => (b.onclick = () => {
  if (b.dataset.confirm && !confirm(b.dataset.confirm)) return;
  act(async () => { await control(b.dataset.cmd); if (b.dataset.cmd.startsWith('calib')) setTimeout(loadSettings, 400); }, 'Done');
}));

const PRESETS = { time: [5, 10, 20, 30, 60], litres: [20, 50, 100, 200, 500] };
function setRunBy(by) {
  runBy = by;
  $$('#runBySeg button').forEach((b) => b.classList.toggle('active', b.dataset.by === by));
  $('#runUnit').textContent = by === 'time' ? 'minutes' : 'litres';
  $('#runAmount').value = PRESETS[by][1];
  $('#presets').innerHTML = PRESETS[by].map((v) => `<button type="button">${v} ${by === 'time' ? 'min' : 'L'}</button>`).join('');
  $$('#presets button').forEach((b, i) => (b.onclick = () => ($('#runAmount').value = PRESETS[by][i])));
}
$$('#runBySeg button').forEach((b) => (b.onclick = () => setRunBy(b.dataset.by)));
setRunBy('time');
$('#startBtn').onclick = () => {
  const v = +$('#runAmount').value;
  if (!(v > 0)) return toast('Enter an amount', true);
  act(() => control('pump_on', runBy === 'time' ? { minutes: v } : { litres: v }), 'Watering started');
};

window.addEventListener('resize', () => { if ($('#tab-history').classList.contains('active')) loadHistory(); });
setInterval(() => { if ($('#tab-history').classList.contains('active')) loadHistory(); }, 300000);
setInterval(() => { if ($('#tab-log').classList.contains('active')) loadLog(); }, 15000);

// ------------------------------------------------------------------ demo simulator
const Demo = (() => {
  const cfg = {
    wifiSsid: 'FarmNet', wifiPass: '********', farmName: 'Green Acres (demo)', lat: 28.61, lon: 77.21,
    moistStart: 35, moistStop: 60, moistCritical: 20, maxRunMin: 45, cycleSoak: true, cycleRunMin: 8, soakMin: 10,
    dailyLimitL: 0, tankMinPct: 10, tankEmptyCm: 100, tankFullCm: 10, tankCapacityL: 1000, weatherEnabled: true,
    rainProbPct: 60, rainMm: 2, lookaheadH: 6, maxWaitH: 10, rainLockoutMin: 90, weatherIntervalMin: 30,
    windowEnabled: true, w1Start: 5, w1End: 10, w2Start: 16, w2End: 20, heatBoost: true, heatTempC: 35, heatBoostPct: 5,
    soilDryRaw: 3800, soilWetRaw: 1500, rainWetRaw: 2800, pumpLpm: 12, dryRunMin: 4, leakPct: 5, relayActiveLow: true,
  };
  const now = () => Math.floor(Date.now() / 1000);
  const d = { mode: 'auto', pump: false, src: 'none', moist: 37, tank: 64, runStart: 0, runL: 0, target: 0, limit: 0,
    today: 42.5, saved: 312, total: 8421, paused: 0, alarms: 0, raining: false, rainAgo: 400, flow: 0, temp: 31, hum: 48, boot: now() };
  const events = [
    [EV(0), 'Controller started (fw 1.0.0)'], [EV(0), 'Weather forecast updated'],
    [EV(1), 'Rain forecast 72% / 4.1 mm within 3 h — holding irrigation'], [EV(0), 'Forecast was right — rain arrived'],
    [EV(0), 'Rain watered the field — saved about 24 L'], [EV(0), 'Auto irrigation complete: 31.4 L, 3 cycle(s)'],
  ].map((e, i) => ({ t: now() - (6 - i) * 5400, up: 0, lvl: e[0], msg: e[1] })).reverse();
  function EV(l) { return l; }
  const log = (lvl, msg) => events.unshift({ t: now(), up: 0, lvl, msg });

  const hProb = [5, 10, 20, 45, 70, 80, 65, 40, 20, 10, 5, 5];
  const hMm = [0, 0, 0.2, 0.8, 2.1, 3.0, 1.4, 0.4, 0, 0, 0, 0];

  setInterval(() => {
    const temp = 31 + 4 * Math.sin(Date.now() / 600000);
    d.flow = d.pump ? cfg.pumpLpm : 0;
    if (d.pump) {
      d.runL += d.flow / 60; d.today += d.flow / 60; d.total += d.flow / 60;
      d.moist = Math.min(80, d.moist + 0.25); d.tank = Math.max(0, d.tank - 0.04);
      const el = now() - d.runStart;
      if ((d.target && d.runL >= d.target) || (d.limit && el >= d.limit) || (d.src === 'auto' && d.moist >= cfg.moistStop)) {
        log(0, `Pump OFF — ${d.runL.toFixed(1)} L in ${fmtDur(el)}`); d.pump = false; d.src = 'none';
      }
    } else {
      d.moist = Math.max(10, d.moist - 0.03);
    }
    d.temp = temp; d.hum = 48 - 6 * Math.sin(Date.now() / 600000);
  }, 1000);

  function status() {
    const fcRain = d.mode === 'auto';
    let code = 0, text = `Soil moisture OK (${d.moist.toFixed(0)}%) — watering starts below ${cfg.moistStart}%`;
    if (d.pump) { code = 1; text = d.src === 'manual' ? `Manual watering — ${d.runL.toFixed(1)} L so far` : 'Auto watering (cycle 1)'; }
    else if (d.mode !== 'auto') text = 'Manual mode — automatic watering is off';
    else if (d.paused > now()) { code = 3; text = `Automation paused — resumes in ${Math.ceil((d.paused - now()) / 60)} min`; }
    else if (d.moist < cfg.moistStart) { code = 2; text = `Rain expected in ~3 h (80%, 7.9 mm) — saving water & power`; }
    return {
      farm: cfg.farmName, fw: '1.0.0', uptime: now() - d.boot + 86400 * 3, time: now(), tz: -new Date().getTimezoneOffset() * 60, heap: 182000,
      sensors: { moisture: d.moist, soilRaw: Math.round(3800 - d.moist * 23), soilOk: true, raining: d.raining, rainRaw: 3990, rainIntensity: 0,
        rainAgoMin: d.rainAgo, temp: d.temp, hum: d.hum, dhtOk: true, tankPct: d.tank, tankL: d.tank * 10, tankCap: 1000,
        distance: 100 - d.tank * 0.9, tankOk: true, flow: d.flow },
      irr: { mode: d.mode, pump: d.pump, source: d.src, status: text, statusCode: code, runSec: now() - d.runStart, runLitres: d.runL,
        targetL: d.target, limitSec: d.limit, session: false, cycle: 0, soaking: false, pausedMin: d.paused > now() ? Math.ceil((d.paused - now()) / 60) : 0,
        effStart: cfg.moistStart, effStop: cfg.moistStop, heatBoost: false, waitingRain: fcRain,
        forecast: { rain: fcRain, prob: 80, mm: 7.9, eta: 3, needProb: 60 }, alarmBits: d.alarms, alarms: [] },
      stats: { total: d.total, today: d.today, saved: d.saved, avgCycle: 24, hits: 7, misses: 2, skips: 13, pumpHours: 41.3, yesterday: 58.2 },
      net: { connected: true, ssid: 'FarmNet', ip: '192.168.1.42', rssi: -61, ap: false, host: 'hybridflow.local' },
      weather: { valid: true, fresh: true, fetched: now() - 600, ageMin: 10, error: '', temp: d.temp - 1, hum: d.hum + 3, wind: 9.4, precip: 0, code: 3,
        hProb, hMm, days: [{ code: 63, max: 34, min: 24, rain: 8.2, prob: 80, et0: 5.1 }, { code: 2, max: 35, min: 25, rain: 0.4, prob: 20, et0: 5.8 },
          { code: 0, max: 37, min: 26, rain: 0, prob: 5, et0: 6.3 }] },
    };
  }

  function history() {
    const t = [], moist = [], tank = [], temp = [], hum = [], rain = [], litres = [];
    const t0 = now() - 287 * 300;
    let m = 52, k = 88;
    for (let i = 0; i < 288; i++) {
      const ts = t0 + i * 300, h = new Date(ts * 1000).getHours();
      const watering = (i > 70 && i < 82) || (i > 200 && i < 210);
      const raining = i > 150 && i < 165;
      m += watering ? 2 : raining ? 1.2 : -0.12; m = Math.max(15, Math.min(75, m));
      k -= watering ? 1.1 : 0; if (i === 240) k = 90;
      t.push(ts); moist.push(+m.toFixed(1)); tank.push(Math.round(k));
      temp.push(+(27 + 7 * Math.sin(((h - 9) / 24) * 2 * Math.PI)).toFixed(1));
      hum.push(Math.round(55 - 15 * Math.sin(((h - 9) / 24) * 2 * Math.PI)));
      rain.push(raining ? 1 : 0); litres.push(watering ? 11 : 0);
    }
    return { t, moist, tank, temp, hum, rain, litres, interval: 300, daily: [58.2, 0, 12.4, 71, 64.3, 0, 0, 49.9, 66.1, 70.2, 38, 55.5, 61, 60] };
  }

  return {
    api(path, body) {
      return new Promise((resolve, reject) => setTimeout(() => {
        if (path === '/api/status') return resolve(status());
        if (path === '/api/settings' && !body) return resolve(Object.assign({}, cfg));
        if (path === '/api/settings') { Object.assign(cfg, body); log(0, 'Settings updated'); return resolve({ ok: true }); }
        if (path === '/api/history') return resolve(history());
        if (path === '/api/events') return resolve(events.slice(0, 50));
        if (path === '/api/control') {
          const c = body.cmd;
          if (c === 'pump_on') {
            if (!d.pump) { d.pump = true; d.runStart = now(); d.runL = 0; }
            d.src = 'manual'; d.target = body.litres || 0; d.limit = body.minutes ? body.minutes * 60 : 0;
            log(0, `Manual watering started: ${body.litres ? body.litres + ' L' : body.minutes + ' min'}`);
          } else if (c === 'pump_off') { if (d.pump) log(0, `Pump OFF (stopped from portal) — ${d.runL.toFixed(1)} L`); d.pump = false; d.src = 'none'; }
          else if (c === 'mode') { d.mode = body.mode; log(0, 'Mode changed to ' + body.mode.toUpperCase()); }
          else if (c === 'pause') { d.paused = now() + body.hours * 3600; log(0, `Automation paused for ${body.hours} h`); }
          else if (c === 'resume') { d.paused = 0; log(0, 'Automation resumed'); }
          else if (c === 'reboot' || c.startsWith('calib') || c === 'reset_stats' || c === 'ack' || c === 'weather_refresh') log(0, 'Demo: ' + c);
          return resolve({ ok: true });
        }
        reject(new Error('Not found'));
      }, 60));
    },
  };
})();

// ------------------------------------------------------------------ boot
if (DEMO) {
  $('#demoBanner').classList.remove('hidden');
  auth = 'demo';
}
(function restoreTab() {
  let t = null;
  try { t = localStorage.getItem('hf_tab'); } catch (e) { /* ignore */ }
  const b = t && document.querySelector(`#tabs button[data-tab="${t}"]`);
  if (b) b.click();
})();
loadSettings();
refreshStatus();
connectWs();
