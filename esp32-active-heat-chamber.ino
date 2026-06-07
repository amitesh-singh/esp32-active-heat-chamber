/**
 * chamber_server.ino
 * ESP32 HTTP REST server for Klipper-controlled heater chamber
 *
 * Endpoints:
 *   POST /heater?state=on|off
 *   POST /temp?target=50.0
 *   POST /fan?speed=0-255
 *   GET  /status   → JSON: temp, humid, setpoint, heater, fan, pid_output, safety_tripped, sensor_fault, mcu_temp, fw_ver, ip
 *
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <WiFiManager.h>

#include "config.h"
#include "heater_controller.h"
#include "pid_controller.h"

#define DEBUG
//#define WEB_OTA
#ifdef WEB_OTA
#include <ElegantOTA.h>
#endif 

#include "oled_display.h"
display *d = nullptr;

#include "automatic_ota.h"

#include <SensirionI2cSht3x.h>
SensirionI2cSht3x sht30;

// ESP-IDF temperature sensor API
extern "C" {
  #include "driver/temperature_sensor.h"
}
temperature_sensor_handle_t temp_sensor = NULL;

void setup_mcu_temp()
{
  temperature_sensor_config_t temp_config = {
    .range_min = -10,  // Minimum expected temperature (°C)
    .range_max = 80    // Maximum expected temperature (°C)
  };

  // Install temperature sensor
  esp_err_t err = temperature_sensor_install(&temp_config, &temp_sensor);
  if (err != ESP_OK) {
    #ifdef DEBUG
    Serial.println("Failed to install temperature sensor");
    #endif
    return;
  }

  // Enable temperature sensor
  err = temperature_sensor_enable(temp_sensor);
  if (err != ESP_OK) {
    #ifdef DEBUG
    Serial.println("Failed to enable temperature sensor");
    #endif
    return;
  }
  #ifdef DEBUG
  Serial.println("Temperature sensor initialized\n");
  #endif
}

heater_controller  hc(HEATER_PIN, HEATER_CYCLE_MS);
pid_controller pid(PID_KP, PID_KI, PID_KD, 0.0f, 1.0f);
WebServer     server(80);
#ifdef WEB_OTA
WebServer ota_server(81);
#endif

//TODO: change it to github later
#define UPDATE_JSON_URL "https://192.168.1.13/firmware.json"

https_ota ota(
    FW_VERSION,
    UPDATE_JSON_URL,
    []() { if (d) d->ota_update_uptodate(); },
    []() { if (d) d->ota_update_install(); }
);

float   currentTemp     = 0.0f;
float   currentHumidity = 0.0f;
float   pidOutput     = 0.0f;
uint8_t fanSpeed      = 0;
bool    heaterEnabled = false;
bool    safetyTripped = false;
bool    sensorOk        = false;
bool    sensorFault     = false;
bool    highTempFault   = false;
bool    forceHeaterStopped = false;
float   mcu_temp = 0.0f;

uint32_t lastPid    = 0;
uint32_t lastSensor = 0;
uint32_t lastOled    = 0;

constexpr const auto HTTP_RESPONSE_200 = 200;
constexpr const auto HTTP_RESPONSE_400 = 400;

bool readSensor()
{
    float t, h;
    uint16_t err = sht30.measureSingleShot(REPEATABILITY_HIGH, false, t, h);
    if (err) {
      #ifdef DEBUG
        Serial.printf("[SHT30] Error: %d\r\n", err);
      #endif  
        sensorFault = true;
        return false;
    }
    sensorFault = false;
    currentTemp     = t;
    currentHumidity = h;
    return true;
}

void applyFanSpeed(uint8_t speed)
{
    fanSpeed = speed;
    ledcWrite(FAN_PWM_PIN, speed);
}

void checkSafety(float t)
{
    if (t >= MAX_TEMP_C || t < -50.0f) {
        hc.emergencyStop();
        applyFanSpeed(255);
        heaterEnabled = false;
        safetyTripped = true;
        #ifdef DEBUG
        Serial.printf("[SAFETY] Tripped at %.1f°C\n", t);
        #endif

    } else {
      //The currentTemp should not go more than 5C than the setTemp
      if (heaterEnabled and currentTemp > pid.getSetpoint() and (currentTemp - pid.getSetpoint()) >= 5) {
        applyFanSpeed(0);
        hc.emergencyStop();
        heaterEnabled = false;
        highTempFault = true;
        
        safetyTripped = true;
        #ifdef DEBUG
        Serial.printf("[SAFETY] currentTemp is higher than setTemp: %.2f : %.2f\r\n",
          currentTemp, pid.getSetpoint());
        #endif 
      }
    }

    if (sensorFault)  {
      hc.emergencyStop();
      applyFanSpeed(0);
      heaterEnabled = false;
      safetyTripped = true;
      #ifdef DEBUG
      Serial.println("[SAFETY] Tripped due to sensor fault");
      #endif
    } else {
      //if the sensor is ok, we can clear the fault;
      //heaterEnabled = true;
      if (!highTempFault)
        safetyTripped = false;
    }
    
    //only clear the fault if externally heater is not stopped
    if (!forceHeaterStopped and highTempFault) {
      //wait for the currentTemp to go down; to restart everything.
      if (currentTemp < pid.getSetpoint() and (pid.getSetpoint() - currentTemp) >= 2) {
        //clear the fault;
        heaterEnabled = true;
        hc.enable();
        pid.reset();
        highTempFault = false;
        safetyTripped = false;
        #ifdef DEBUG
        Serial.printf("[SAFETY] cleared the fault:\r\n");
        #endif
      }
    }
}

String statusJson()
{
    char buf[256];
    String ip = WiFi.localIP().toString();
    constexpr auto *t = "true";
    constexpr auto *f = "false";

    snprintf(buf, sizeof(buf),
        "{\"temp\":%.2f,\"humid\":%.2f,\"setpoint\":%.1f,\"heater\":%s,"
        "\"fan\":%d,\"pid_output\":%.3f,\"safety_tripped\":%s,"
        "\"sensor_fault\":%s,"
        "\"mcu_temp\":%.2f,"
        "\"fw_ver\":\"%s\","
        "\"ip\":\"%s\"}",
        currentTemp, currentHumidity, pid.getSetpoint(),
        heaterEnabled ? t : f,
        fanSpeed, pidOutput,
        safetyTripped ? t : f,
        sensorFault ? t : f,
        mcu_temp,
        FW_VERSION,
        ip.c_str());

    return String(buf);
}

// GET /status
void handleStatus()
{
    server.send(HTTP_RESPONSE_200, "application/json", statusJson());
}

// POST /heater?state=on|off
void handleHeater()
{
    if (!server.hasArg("state")) {
        server.send(HTTP_RESPONSE_400, "application/json", "{\"error\":\"missing state\"}"); return;
    }
    String state = server.arg("state");
    if (state == "on" && !safetyTripped) {
        forceHeaterStopped = false;
        heaterEnabled = true;
        hc.enable();
        pid.reset();
        #ifdef DEBUG
        Serial.println("[HTTP] Heater ON");
        #endif
    } else if (state == "off") {
        forceHeaterStopped = true;
        heaterEnabled = false;
        //we clear the hightemp fault too;
        highTempFault = false;
        safetyTripped = false;
        hc.disable();
        applyFanSpeed(0);
        pidOutput = 0;
        pid.reset();
        #ifdef DEBUG
        Serial.println("[HTTP] Heater OFF");
        #endif
    } else {
        server.send(HTTP_RESPONSE_400, "application/json", "{\"error\":\"invalid state\"}"); return;
    }
    server.send(HTTP_RESPONSE_200, "application/json", statusJson());
}

// POST /temp?target=50.0
void handleTemp() 
{
    if (!server.hasArg("target")) {
        server.send(HTTP_RESPONSE_400, "application/json", "{\"error\":\"missing target\"}"); return;
    }
    float target = server.arg("target").toFloat();
    if (target < 20.0f || target > 80.0f) {
        server.send(HTTP_RESPONSE_400, "application/json", "{\"error\":\"range 20-80C\"}"); return;
    }
    pid.setSetpoint(target);
    pid.reset();
    #ifdef DEBUG
    Serial.printf("[HTTP] Setpoint → %.1f°C\n", target);
    #endif
    server.send(HTTP_RESPONSE_200, "application/json", statusJson());
}

// POST /fan?speed=0-255
static void handleFan()
{
    if (!server.hasArg("speed")) {
        server.send(HTTP_RESPONSE_400, "application/json", "{\"error\":\"missing speed\"}"); return;
    }
    int speed = constrain(server.arg("speed").toInt(), 0, 255);
    applyFanSpeed((uint8_t)speed);
    #ifdef DEBUG
    Serial.printf("[HTTP] Fan → %d\n", speed);
    #endif
    server.send(HTTP_RESPONSE_200, "application/json", statusJson());
}

static void setupWifi()
{
  WiFi.mode(WIFI_STA);
  // Get the unique chip ID (48-bit MAC)
  uint64_t chipid = ESP.getEfuseMac();

  // Format it into a readable hex string
  char ssid[40];
  sprintf(ssid, "HeaterChamber-%04X", (uint16_t)(chipid & 0xFFFF));

  // Initialize WiFiManager with custom SSID
  WiFiManager wifi_manager;
  wifi_manager.autoConnect(ssid);

  #ifdef DEBUG
  Serial.printf("\n[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.print("Access Point SSID: ");
  Serial.println(ssid);
  #endif
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() 
{  
  #ifdef DEBUG
  Serial.begin(115200);
  delay(100);
  #endif
  
  hc.begin();
  hc.disable();
  pid.setSetpoint(DEFAULT_TARGET);

  setup_mcu_temp();

  ledcAttach(FAN_PWM_PIN, FAN_FREQ_HZ, FAN_RESOLUTION);
  applyFanSpeed(0);


  //analogSetAttenuation(ADC_11db);
  //analogReadResolution(12);

  setupWifi();
  
  // SHT30
  Wire.begin();
  delay(50);
  sht30.begin(Wire, SHT30_I2C_ADDR_44);

  d = new display();
  d->begin();
  d->ota_update();
  ota.checkAndUpdate();

  server.on("/", HTTP_GET, []() {
  server.send(HTTP_RESPONSE_200, "text/html", R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Heater Chamber</title>
<style>
  :root {
    --bg:       #0f1117;
    --surface:  #1a1d27;
    --border:   #2a2d3a;
    --text:     #e2e4ed;
    --muted:    #6b7080;
    --accent:   #f59e0b;
    --red:      #ef4444;
    --green:    #22c55e;
    --blue:     #3b82f6;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: ui-monospace, 'Cascadia Code', monospace;
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 24px 16px;
  }
  h1 {
    font-size: 13px;
    letter-spacing: .12em;
    text-transform: uppercase;
    color: var(--muted);
    margin-bottom: 24px;
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 12px;
    width: 100%;
    max-width: 480px;
  }
  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 18px 20px;
  }
  .card.wide { grid-column: 1 / -1; }
  .label {
    font-size: 10px;
    letter-spacing: .1em;
    text-transform: uppercase;
    color: var(--muted);
    margin-bottom: 6px;
  }
  .value {
    font-size: 36px;
    font-weight: 600;
    line-height: 1;
    color: var(--text);
  }
  .value.hot  { color: var(--accent); }
  .value.cool { color: var(--blue); }
  .unit {
    font-size: 14px;
    color: var(--muted);
    margin-left: 3px;
  }
  .badge {
    display: inline-block;
    padding: 3px 10px;
    border-radius: 4px;
    font-size: 11px;
    font-weight: 600;
    letter-spacing: .06em;
    text-transform: uppercase;
    white-space: normal;
    word-break: break-word;
    max-width: 100%;
  }
  .badge.on  { background: #14532d; color: var(--green); }
  .badge.off { background: #1f1f1f; color: var(--muted); }
  .badge.warn { background: #450a0a; color: var(--red); }

  /* Arc gauge */
  .gauge-wrap {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
  }
  .arc-svg { overflow: visible; }
  .arc-track { fill: none; stroke: var(--border); stroke-width: 8; }
  .arc-fill  { fill: none; stroke-width: 8; stroke-linecap: round;
               transition: stroke-dashoffset .6s ease, stroke .4s; }
  .arc-text  { font-family: inherit; font-weight: 600; fill: var(--text); }
  .arc-sub   { font-family: inherit; fill: var(--muted); }

  /* Fan bar */
  .fan-bar-wrap { margin-top: 8px; }
  .fan-bar-bg {
    height: 6px;
    background: var(--border);
    border-radius: 3px;
    overflow: hidden;
    margin-top: 8px;
  }
  .fan-bar-fill {
    height: 100%;
    background: var(--blue);
    border-radius: 3px;
    transition: width .5s ease;
  }
  .fan-pct { font-size: 28px; font-weight: 600; color: var(--blue); }

  /* Controls */
  .controls { display: flex; flex-direction: column; gap: 10px; }
  .row { display: flex; gap: 8px; align-items: center; }
  input[type=number] {
    flex: 1;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--text);
    font-family: inherit;
    font-size: 14px;
    padding: 8px 10px;
    outline: none;
  }
  input[type=number]:focus { border-color: var(--accent); }
  button {
    padding: 8px 14px;
    border: none;
    border-radius: 6px;
    font-family: inherit;
    font-size: 12px;
    font-weight: 600;
    letter-spacing: .05em;
    cursor: pointer;
    transition: opacity .15s;
  }
  button:hover { opacity: .85; }
  button:active { opacity: .7; }
  .btn-on  { background: #065f46; color: var(--green); }
  .btn-off { background: #1f1f1f; color: var(--muted); border: 1px solid var(--border); }
  .btn-set { background: #78350f; color: var(--accent); }
  .btn-fan { background: #1e3a5f; color: var(--blue); }

  /* Status strip */
  .status-strip {
    margin-top: 12px;
    width: 100%;
    max-width: 480px;
    font-size: 10px;
    letter-spacing: .06em;
    color: var(--muted);
    text-align: center;
  }
  
  #ipAddr {
  display: inline-block;
  margin-top: 4px;
  font-size: 13px;   /* increase this if you want bigger */
  font-weight: 600;
  color: var(--blue);
}

  #mcuTemp {
  display: inline-block;
  margin-top: 4px;
  font-size: 13px;   /* increase this if you want bigger */
  font-weight: 600;
  color: var(--red);

#ipAddr a {
  color: var(--blue);
  text-decoration: none;
}

#ipAddr a:hover {
  text-decoration: underline;
}
  .dot {
    display: inline-block;
    width: 6px; height: 6px;
    border-radius: 50%;
    margin-right: 5px;
    background: var(--green);
    animation: pulse 2s infinite;
  }
  .dot.stale { background: var(--red); animation: none; }
  @keyframes pulse {
    0%,100%{ opacity:1; } 50%{ opacity:.4; }
  }
</style>
</head>
<body>

<h1>&#9632; Heater Chamber</h1>

<div class="grid">

  <!-- Temperature gauge -->
  <div class="card">
    <div class="label">Chamber temp</div>
    <div class="gauge-wrap">
      <svg class="arc-svg" width="120" height="72" viewBox="-10 0 120 72">
        <path class="arc-track" d="M5,65 A55,55 0 0,1 95,65"/>
        <path class="arc-fill" id="tempArc" d="M5,65 A55,55 0 0,1 95,65"
              stroke="#f59e0b"
              stroke-dasharray="0 173"/>
        <text class="arc-text" id="tempVal" x="50" y="58"
              text-anchor="middle" font-size="22">--</text>
        <text class="arc-sub" x="50" y="70"
              text-anchor="middle" font-size="9">°C</text>
      </svg>
    </div>
  </div>

  <!-- Humidity -->
  <div class="card">
    <div class="label">Humidity</div>
    <div class="gauge-wrap">
      <svg class="arc-svg" width="120" height="72" viewBox="-10 0 120 72">
        <path class="arc-track" d="M5,65 A55,55 0 0,1 95,65"/>
        <path class="arc-fill" id="humArc" d="M5,65 A55,55 0 0,1 95,65"
              stroke="#3b82f6"
              stroke-dasharray="0 173"/>
        <text class="arc-text" id="humVal" x="50" y="58"
              text-anchor="middle" font-size="22">--</text>
        <text class="arc-sub" x="50" y="70"
              text-anchor="middle" font-size="9">%RH</text>
      </svg>
    </div>
  </div>

  <!-- Setpoint -->
  <div class="card">
    <div class="label">Setpoint</div>
    <div class="value" id="spVal">--<span class="unit">°C</span></div>
    <div style="margin-top:10px">
      <div class="label" style="margin-top:8px">PID output</div>
      <div style="font-size:20px;font-weight:600;color:var(--accent)" id="pidVal">--%</div>
    </div>
  </div>

  <!-- Fan -->
  <div class="card">
    <div class="label">Fan speed</div>
    <div class="fan-pct" id="fanPct">--%</div>
    <div class="fan-bar-bg">
      <div class="fan-bar-fill" id="fanBar" style="width:0%"></div>
    </div>
    <div style="margin-top:6px;font-size:10px;color:var(--muted)" id="fanRaw">-- / 255</div>
  </div>

  <!-- Heater state -->
  <div class="card">
    <div class="label">Heater</div>
    <div id="heaterBadge" class="badge off">OFF</div>
    <div style="margin-top:12px">
      <div class="label">Safety</div>
      <div id="safetyBadge" class="badge on">OK</div>
    </div>
  </div>
  <!-- Fault -->
  <div id="faultBanner" style="
    display: none;
    grid-column: 1 / -1;
    background: #450a0a;
    color: #ef4444;
    border: 1px solid #7f1d1d;
    border-radius: 8px;
    padding: 10px 14px;
    font-size: 11px;
    letter-spacing: .06em;
    text-transform: uppercase;
    font-weight: 600;">
    &#9888; Sensor fault — check I2C wiring (SDA: GPIO8, SCL: GPIO9)
</div>

  <!-- Controls -->
  <div class="card wide controls">
    <div class="label">Controls</div>

    <div class="row">
      <button class="btn-on"  onclick="heater('on')">Heater ON</button>
      <button class="btn-off" onclick="heater('off')">Heater OFF</button>
    </div>

    <div class="row">
      <input type="number" id="tempInput" placeholder="Target °C" min="20" max="80" step="1">
      <button class="btn-set" onclick="setTemp()">Set Temp</button>
    </div>

    <div class="row">
      <input type="number" id="fanInput" placeholder="Fan 0–255" min="0" max="255" step="1">
      <button class="btn-fan" onclick="setFan()">Set Fan</button>
    </div>
  </div>

</div>

<div class="status-strip">
  <span class="dot" id="dot"></span>
  <span id="lastUpdate">connecting...</span>
  <br>
  <span id="ipAddr">--</span>
  <br>
  <span id="mcuTemp">esp32 temp: --</span>
  <br>
  <span id="fwVer">v--</span>
</div>

<script>
const ARC_LEN = 173;

function setArc(pct) {
  const fill = Math.max(0, Math.min(1, pct));
  const el = document.getElementById('tempArc');
  el.setAttribute('stroke-dasharray', `${fill * ARC_LEN} ${ARC_LEN}`);
  el.setAttribute('stroke', fill > .75 ? '#ef4444' : fill > .4 ? '#f59e0b' : '#3b82f6');
}

function update(d) {
  const fault = d.sensor_fault === true || d.sensor_fault === 'true';

  // ── Fault banner + button disable ──────────────────────────────────────────
  document.getElementById('faultBanner').style.display = fault ? 'block' : 'none';
  document.querySelectorAll('button').forEach(b => b.disabled = fault);

  // ── Temp + humidity ────────────────────────────────────────────────────────
  document.getElementById('tempVal').textContent = fault ? '--'   : parseFloat(d.temp).toFixed(1);
  document.getElementById('spVal').innerHTML     = fault ? '--'   : parseFloat(d.setpoint).toFixed(1) + '<span class="unit">°C</span>';
  document.getElementById('pidVal').textContent  = fault ? '--%'  : (parseFloat(d.pid_output) * 100).toFixed(1) + '%';
  
  // ── Humidity arc ───────────────────────────────────────────────────────────
  const humArc = document.getElementById('humArc');
  if (fault) {
    humArc.setAttribute('stroke-dasharray', '0 173');
    humArc.setAttribute('stroke', '#ef4444');
    document.getElementById('humVal').textContent = '--';
  } else {
    const humPct = Math.max(0, Math.min(1, parseFloat(d.humid) / 100));
    humArc.setAttribute('stroke-dasharray', `${(humPct * 173).toFixed(1)} 173`);
    humArc.setAttribute('stroke', humPct > 0.7 ? '#ef4444' : humPct > 0.4 ? '#3b82f6' : '#22c55e');
    document.getElementById('humVal').textContent = parseFloat(d.humid).toFixed(1);
  }

  // ── Arc gauge ──────────────────────────────────────────────────────────────
  if (fault) {
    document.getElementById('tempArc').setAttribute('stroke-dasharray', `0 173`);
    document.getElementById('tempArc').setAttribute('stroke', '#ef4444');
  } else {
    setArc((parseFloat(d.temp) - 20) / 65);
  }

  // ── Fan ────────────────────────────────────────────────────────────────────
  const fanPct = Math.round(parseFloat(d.fan) / 255 * 100);
  document.getElementById('fanPct').textContent  = fanPct + '%';
  document.getElementById('fanBar').style.width  = fanPct + '%';
  document.getElementById('fanRaw').textContent  = d.fan + ' / 255';

  // ── Badges ─────────────────────────────────────────────────────────────────
  const heaterOn  = d.heater === true       || d.heater === 'true';
  const safetyOn  = d.safety_tripped === true || d.safety_tripped === 'true';
  const sensorFault = d.sensor_fault === true || d.sensor_fault === 'true';
  const hBadge = document.getElementById('heaterBadge');
  hBadge.textContent = heaterOn ? 'ON' : 'OFF';
  hBadge.className   = 'badge ' + (heaterOn ? 'on' : 'off');

  const sBadge = document.getElementById('safetyBadge');
  sBadge.textContent = sensorFault ? 'SENSOR FAULT,CHECK WIRING' : (safetyOn ? 'TRIPPED,TEMP HIGH BY >5C' : 'OK');
 
  sBadge.className   = 'badge ' + (sensorFault || safetyOn ? 'warn' : 'on');
  
  const onBtn  = document.querySelector('.btn-on');
  const offBtn = document.querySelector('.btn-off');

  if (heaterOn) {
    onBtn.style.background  = '#065f46';
    onBtn.style.color       = '#22c55e';

    offBtn.style.background = '#1f1f1f';
    offBtn.style.color      = '#6b7080';
  } else {
    offBtn.style.background = '#065f46';
    offBtn.style.color      = '#22c55e';

    onBtn.style.background  = '#1f1f1f';
    onBtn.style.color       = '#6b7080';
  }
  // ── Status dot ─────────────────────────────────────────────────────────────
  document.getElementById('dot').className = 'dot';
  document.getElementById('lastUpdate').textContent = 'updated ' + new Date().toLocaleTimeString();
  document.getElementById('ipAddr').textContent = d.ip;
  //document.getElementById('ipAddr').innerHTML =
  //`OTA: <a href="http://${d.ip}:81/update" target="_blank">${d.ip}</a>`;

  document.getElementById('mcuTemp').textContent = `esp32 temp: ${d.mcu_temp}°C`;
  document.getElementById('fwVer').textContent = `v${d.fw_ver}`;
  }
  
async function poll() {
  try {
    const r = await fetch('/status');
    const d = await r.json();
    update(d);
  } catch(e) {
    document.getElementById('dot').className = 'dot stale';
    document.getElementById('lastUpdate').textContent = 'connection lost';
  }
}

async function heater(state) {
  await fetch('/heater?state=' + state, { method: 'POST' });
  poll();
}

async function setTemp() {
  const v = document.getElementById('tempInput').value;
  if (!v) return;
  await fetch('/temp?target=' + v, { method: 'POST' });
  poll();
}

async function setFan() {
  const v = document.getElementById('fanInput').value;
  if (v === '') return;
  await fetch('/fan?speed=' + v, { method: 'POST' });
  poll();
}
poll();
setInterval(poll, 2000);
</script>
</body>
</html>
)rawhtml");
});


    server.on("/status", HTTP_GET,  handleStatus);
    server.on("/heater", HTTP_POST, handleHeater);
    server.on("/temp",   HTTP_POST, handleTemp);
    server.on("/fan",    HTTP_POST, handleFan);
    server.onNotFound([](){
        server.send(404, "application/json", "{\"error\":\"not found\"}");
    });
    server.begin();
#ifdef DEBUG
    Serial.println("[HTTP] Server ready at port 80");
#endif

#ifdef WEB_OTA
    ElegantOTA.begin(&ota_server); 
    ota_server.on("/", []() {
        ota_server.send(HTTP_RESPONSE_200, "text/plain", "Hi! Active Heater Chamber v 0.1, http://<ip_addr>:81/update for OTA");
    });
    ota_server.begin();
    #ifdef DEBUG
    Serial.println("[OTA] ota server ready at port 81");
    #endif
#endif 
  
    d->setIP(WiFi.localIP().toString().c_str());

    //just read mcu temp once
    temperature_sensor_get_celsius(temp_sensor, &mcu_temp);

    delay(300);
}

uint32_t now = 0;
uint8_t fan_speed = 0;
uint32_t last_mcu_temp = 0;

void loop() 
{
    server.handleClient();

#ifdef WEB_OTA
    ota_server.handleClient();
#endif 
    
    now = millis();

    if (now - lastSensor >= 1000) {
        lastSensor = now;
        readSensor();
       
        checkSafety(currentTemp);
    }

    if (heaterEnabled && !safetyTripped && (now - lastPid >= 500)) {
        lastPid   = now;
        pidOutput = pid.compute(currentTemp);
        hc.setPower(pidOutput);
        // Auto fan scales 55-255 with heater power
        // calibrated for the fan to actually start spinning at around 25% power, and be at full blast at 100% power
        fan_speed = (uint8_t)(55 + (255 - 55) * pidOutput);
        applyFanSpeed(fan_speed);
    }

    hc.update();

    if (now - lastOled >= 500) {
        lastOled = now;
        #ifdef DEBUG
        Serial.println(statusJson());
        Serial.flush();
        #endif
        d->update(currentTemp, currentHumidity, pid.getSetpoint(),
          heaterEnabled, fan_speed, pidOutput, sensorFault || highTempFault);
    }

    //update mcu temp every 1 minute only
    if (now - last_mcu_temp >= 1000*60) {
      last_mcu_temp = now;
      esp_err_t err = temperature_sensor_get_celsius(temp_sensor, &mcu_temp);
      #ifdef DEBUG
      if (err == ESP_OK) {
        Serial.printf("MCU Temperature: %.2f °C\r\n", mcu_temp);
      } else {
        Serial.println("Failed to read temperature");
      }
      #endif
    }
}
