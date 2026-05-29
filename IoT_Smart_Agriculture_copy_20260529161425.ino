/*
 * ============================================================
 *  IoT Smart Irrigation System — ESP32 NodeMCU Type-C
 *  Jigyasu — Learning by Doing
 * ============================================================
 *  Sensors  : DHT11, DS18B20 (waterproof probe), Rain Sensor,
 *             Water Level Sensor, LDR Module, Soil Moisture
 *  Display  : OLED 1.3" (SH1106 128x64, I2C)
 *  Actuator : Relay Module (Water Pump)
 *  UI       : Local web server at 192.168.4.1 (AP Mode)
 *
 *  PIN MAPPING:
 *  ┌─────────────────┬──────────┐
 *  │ DHT11           │ GPIO 4   │
 *  │ DS18B20         │ GPIO 5   │ (needs 4.7k pull-up to 3.3V)
 *  │ Rain Sensor DO  │ GPIO 34  │
 *  │ Water Sensor AO │ GPIO 32  │
 *  │ LDR AO          │ GPIO 33  │
 *  │ Soil Moisture   │ GPIO 35  │
 *  │ Relay IN        │ GPIO 26  │
 *  │ OLED SDA        │ GPIO 21  │
 *  │ OLED SCL        │ GPIO 22  │
 *  └─────────────────┴──────────┘
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <ArduinoJson.h>

// ==================== PIN DEFINITIONS ====================
#define DHT_PIN          4
#define DHT_TYPE         DHT11
#define DS18B20_PIN      5
#define RAIN_DO_PIN      34
#define WATER_AO_PIN     32
#define LDR_AO_PIN       33
#define SOIL_AO_PIN      35
#define RELAY_PIN        26

// Relay: true = LOW turns pump ON, false = HIGH turns pump ON
#define RELAY_ACTIVE_LOW true

// ==================== CALIBRATION ====================
// Soil Moisture Calibration (find YOUR values via Serial Monitor)
#define SOIL_RAW_WET      1500   // ADC when sensor in water → 100% moisture
#define SOIL_RAW_DRY      3500   // ADC when sensor in dry air → 0% moisture

// Pump activates when moisture % falls BELOW this value
#define SOIL_DRY_PCT_THRESHOLD  30    // 0-100% - pump ON when moisture < 30%

// Water tank threshold (ADC below this = empty)
#define WATER_EMPTY_THRESHOLD  500

// DS18B20 safety threshold (°C) - pump won't run above this
#define DS18B20_HOT_THRESHOLD   35.0

// ==================== WiFi AP ====================
const char* AP_SSID = "JigyasuIrrigation";
const char* AP_PASS = "jigyasu123";

// ==================== GLOBAL OBJECTS ====================
DHT dht(DHT_PIN, DHT_TYPE);
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
WebServer server(80);

// ==================== DATA STRUCTURE ====================
struct SensorData {
  float temperature;
  float humidity;
  float probeTemp;
  bool  probeTempValid;
  bool  rainDetected;
  int   waterLevelRaw;
  int   waterLevelPct;
  int   ldrRaw;
  int   lightPct;
  int   soilRaw;
  int   soilMoisturePct;
  bool  pumpOn;
  unsigned long lastUpdate;
};
SensorData data;

// OLED page cycling
uint8_t  oledPage = 0;
uint32_t lastPageSwitch = 0;
const uint8_t  TOTAL_PAGES = 5;
const uint32_t PAGE_INTERVAL_MS = 3000;
uint32_t lastSensorRead = 0;
const uint16_t SENSOR_INTERVAL_MS = 2000;

// ==================== HELPER FUNCTIONS ====================
int adcToPct(int raw, int rawMin, int rawMax, bool invert = false) {
  int pct = map(constrain(raw, rawMin, rawMax), rawMin, rawMax, 0, 100);
  return invert ? 100 - pct : pct;
}

void setPump(bool on) {
  data.pumpOn = on;
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? (!on) : on);
  Serial.print(">>> PUMP: ");
  Serial.println(on ? "ON 💧" : "OFF ⛔");
}

// ==================== READ ALL SENSORS ====================
void readSensors() {
  // DHT11
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) data.temperature = t;
  if (!isnan(h)) data.humidity = h;

  // DS18B20
  ds18b20.requestTemperatures();
  float pt = ds18b20.getTempCByIndex(0);
  if (pt != DEVICE_DISCONNECTED_C && pt > -100.0) {
    data.probeTemp = pt;
    data.probeTempValid = true;
  } else {
    data.probeTempValid = false;
  }

  // Rain sensor (LOW = rain detected)
  data.rainDetected = (digitalRead(RAIN_DO_PIN) == LOW);

  // Water level
  data.waterLevelRaw = analogRead(WATER_AO_PIN);
  data.waterLevelPct = adcToPct(data.waterLevelRaw, 0, 4095);

  // LDR (inverted: high raw = dark)
  data.ldrRaw = analogRead(LDR_AO_PIN);
  data.lightPct = adcToPct(data.ldrRaw, 0, 4095, true);

  // Soil moisture (high raw = dry, so invert)
  data.soilRaw = analogRead(SOIL_AO_PIN);
  data.soilMoisturePct = adcToPct(data.soilRaw, SOIL_RAW_WET, SOIL_RAW_DRY, true);

  data.lastUpdate = millis();

  // Debug output
  Serial.println("------------------------------------");
  Serial.print("🌱 Soil: RAW=");
  Serial.print(data.soilRaw);
  Serial.print(" | Moisture=");
  Serial.print(data.soilMoisturePct);
  Serial.println("%");
  Serial.print("💧 Water Tank: RAW=");
  Serial.print(data.waterLevelRaw);
  Serial.print(" | Level=");
  Serial.print(data.waterLevelPct);
  Serial.println("%");
  Serial.print("☔ Rain: ");
  Serial.println(data.rainDetected ? "YES" : "NO");
  Serial.print("🌡️ DS18B20: ");
  if (data.probeTempValid) {
    Serial.print(data.probeTemp);
    Serial.println(" °C");
  } else {
    Serial.println("NOT CONNECTED");
  }
}

// ==================== AUTO IRRIGATION LOGIC ====================
void autoIrrigate() {
  // Check conditions for pump activation
  bool soilNeedsWater = (data.soilMoisturePct < SOIL_DRY_PCT_THRESHOLD);
  bool tankHasWater   = (data.waterLevelRaw > WATER_EMPTY_THRESHOLD);
  bool isRaining      = data.rainDetected;
  bool isTooHot       = (data.probeTempValid && data.probeTemp > DS18B20_HOT_THRESHOLD);

  // Detailed debug output
  Serial.println("========== AUTO IRRIGATION CHECK ==========");
  Serial.printf("🌱 Soil Moisture: %d%% ", data.soilMoisturePct);
  Serial.println(soilNeedsWater ? "→ DRY ✅" : "→ WET ❌");
  Serial.printf("💧 Water Tank: %d%% (%s)\n", data.waterLevelPct, tankHasWater ? "OK ✅" : "EMPTY ❌");
  Serial.printf("☔ Rain: %s\n", isRaining ? "YES ❌" : "NO ✅");
  Serial.printf("🌡️ Probe Temp: %s\n", isTooHot ? "HOT ❌" : "OK ✅");
  
  // Decision: pump ON only if soil is DRY and all other conditions are favorable
  bool pumpShouldRun = soilNeedsWater && tankHasWater && !isRaining && !isTooHot;
  
  Serial.printf("🎯 DECISION: Pump %s\n", pumpShouldRun ? "ON 💧" : "OFF ⛔");
  Serial.println("========================================\n");
  
  setPump(pumpShouldRun);
}

// ==================== OLED DISPLAY ====================
void drawProgressBar(int x, int y, int w, int h, int pct) {
  u8g2.drawFrame(x, y, w, h);
  int fill = map(constrain(pct, 0, 100), 0, 100, 0, w - 2);
  if (fill > 0) u8g2.drawBox(x + 1, y + 1, fill, h - 2);
}

void updateOLED() {
  if (millis() - lastPageSwitch > PAGE_INTERVAL_MS) {
    oledPage = (oledPage + 1) % TOTAL_PAGES;
    lastPageSwitch = millis();
  }

  char buf[28];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawBox(0, 0, 128, 11);
  u8g2.setDrawColor(0);
  u8g2.drawStr(2, 9, "Jigyasu Irrigation");
  u8g2.setDrawColor(1);

  // Pump indicator
  if (data.pumpOn) u8g2.drawDisc(122, 5, 3);
  else u8g2.drawCircle(122, 5, 3);

  switch (oledPage) {
    case 0:  // Air Temp & Humidity
      u8g2.setFont(u8g2_font_8x13B_tf);
      snprintf(buf, sizeof(buf), "%.1f C", data.temperature);
      u8g2.drawStr(4, 32, buf);
      snprintf(buf, sizeof(buf), "%.1f %%", data.humidity);
      u8g2.drawStr(4, 48, buf);
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr(75, 32, "Air");
      u8g2.drawStr(75, 48, "Humidity");
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(2, 63, "[1/5] Environment");
      break;

    case 1:  // DS18B20 Probe
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr(4, 18, "Water/Soil Probe");
      u8g2.setFont(u8g2_font_8x13B_tf);
      if (data.probeTempValid) {
        snprintf(buf, sizeof(buf), "%.2f C", data.probeTemp);
        u8g2.drawStr(4, 36, buf);
      } else {
        u8g2.drawStr(4, 36, "No Probe");
      }
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(2, 63, "[2/5] DS18B20");
      break;

    case 2:  // Soil & Light
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr(2, 20, "Soil");
      snprintf(buf, sizeof(buf), "%d%%", data.soilMoisturePct);
      u8g2.drawStr(100, 20, buf);
      drawProgressBar(2, 22, 124, 7, data.soilMoisturePct);
      u8g2.drawStr(2, 38, "Light");
      snprintf(buf, sizeof(buf), "%d%%", data.lightPct);
      u8g2.drawStr(100, 38, buf);
      drawProgressBar(2, 40, 124, 7, data.lightPct);
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(2, 63, "[3/5] Soil+Light");
      break;

    case 3:  // Water & Rain
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr(2, 20, "Water Tank");
      snprintf(buf, sizeof(buf), "%d%%", data.waterLevelPct);
      u8g2.drawStr(100, 20, buf);
      drawProgressBar(2, 22, 124, 7, data.waterLevelPct);
      u8g2.drawStr(2, 38, "Rain");
      u8g2.drawStr(100, 38, data.rainDetected ? "YES" : "NO");
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(2, 63, "[4/5] Water");
      break;

    case 4:  // Pump
      u8g2.setFont(u8g2_font_7x14B_tf);
      u8g2.drawStr(8, 30, data.pumpOn ? "PUMP: ON" : "PUMP: OFF");
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(8, 44, "Mode: AUTO");
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr(2, 56, "Web: 192.168.4.1");
      u8g2.drawStr(2, 64, "[5/5] Status");
      break;
  }
  u8g2.sendBuffer();
}

// ==================== ELEGANT MINIMALIST WEB PAGE ====================
String buildPage() {
  String html;
  html = "<!DOCTYPE html><html>";
  html += "<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Jigyasu | Smart Irrigation</title>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Inter:ital,wght@0,300;0,400;0,500;0,600;1,300;1,400&display=swap' rel='stylesheet'>";
  html += "<style>";
  html += ":root{--bg:#fafaf9;--surface:#ffffff;--border:#e5e5e0;--text:#1c1c1a;--muted:#78786a;--accent:#2d6a4f;--accent-light:#40916c;--danger:#e63946;--warm:#f4a261;--radius:20px;--shadow:0 1px 3px rgba(0,0,0,0.05),0 0 0 1px rgba(0,0,0,0.02)}";
  html += "*{margin:0;padding:0;box-sizing:border-box}";
  html += "body{background:var(--bg);color:var(--text);font-family:'Inter',-apple-system,BlinkMacSystemFont,sans-serif;font-weight:400;line-height:1.5;padding:24px 16px}";
  html += ".container{max-width:680px;margin:0 auto}";
  html += ".logo{text-align:center;margin-bottom:32px}";
  html += ".logo h1{font-size:2rem;font-weight:500;letter-spacing:-0.02em;background:linear-gradient(135deg,#2d6a4f,#52b788);-webkit-background-clip:text;background-clip:text;color:transparent;margin-bottom:4px}";
  html += ".logo .tagline{font-size:0.75rem;color:var(--muted);letter-spacing:0.3em;text-transform:uppercase}";
  html += ".status-badge{display:inline-flex;align-items:center;gap:8px;background:var(--surface);border-radius:99px;padding:8px 16px;margin-bottom:24px;border:1px solid var(--border);font-size:0.8rem}";
  html += ".pulse{width:8px;height:8px;border-radius:50%;background:var(--accent);animation:pulse 1.5s ease infinite}";
  html += "@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}";
  html += ".card{background:var(--surface);border-radius:var(--radius);padding:20px;margin-bottom:12px;border:1px solid var(--border);transition:all 0.2s ease}";
  html += ".card-header{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:16px}";
  html += ".card-header h3{font-size:0.7rem;font-weight:500;text-transform:uppercase;letter-spacing:0.08em;color:var(--muted)}";
  html += ".card-header .value-large{font-size:2rem;font-weight:500;letter-spacing:-0.02em;color:var(--text)}";
  html += ".grid-2{display:grid;grid-template-columns:1fr 1fr;gap:12px}";
  html += ".metric{display:flex;justify-content:space-between;align-items:baseline;padding:12px 0;border-bottom:1px solid var(--border)}";
  html += ".metric:last-child{border-bottom:none}";
  html += ".metric-label{font-size:0.8rem;color:var(--muted)}";
  html += ".metric-value{font-size:1.1rem;font-weight:500}";
  html += ".progress{background:var(--bg);border-radius:99px;height:6px;margin-top:8px;overflow:hidden}";
  html += ".progress-fill{background:var(--accent);height:100%;border-radius:99px;transition:width 0.5s cubic-bezier(0.2,0.9,0.4,1.1)}";
  html += ".pump-card{background:linear-gradient(135deg,#f8f9fa,#ffffff);border-left:3px solid var(--accent)}";
  html += ".pump-status{font-size:1.2rem;font-weight:500;margin:8px 0}";
  html += ".badge{display:inline-block;padding:2px 10px;border-radius:99px;font-size:0.7rem;font-weight:500}";
  html += ".badge-success{background:#e8f5ee;color:#2d6a4f}";
  html += ".badge-warning{background:#fff3e0;color:#e67e22}";
  html += ".badge-danger{background:#fee;color:var(--danger)}";
  html += ".badge-info{background:#e8f0fe;color:#1a73e8}";
  html += ".footer{text-align:center;margin-top:32px;font-size:0.7rem;color:var(--muted)}";
  html += "@media(max-width:480px){.grid-2{grid-template-columns:1fr}}";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  
  // Logo Section
  html += "<div class='logo'>";
  html += "<h1>Jigyasu</h1>";
  html += "<div class='tagline'>Learning by Doing</div>";
  html += "</div>";
  
  // Status
  html += "<div style='display:flex;justify-content:center;margin-bottom:24px'>";
  html += "<div class='status-badge'><span class='pulse'></span><span>System Online · Auto Mode</span></div>";
  html += "</div>";
  
  // Environment
  html += "<div class='grid-2'>";
  html += "<div class='card'><div class='card-header'><h3>Temperature</h3><span class='value-large' id='temp'>--</span><span style='font-size:0.9rem'>°C</span></div></div>";
  html += "<div class='card'><div class='card-header'><h3>Humidity</h3><span class='value-large' id='hum'>--</span><span style='font-size:0.9rem'>%</span></div></div>";
  html += "</div>";
  
  // Probe
  html += "<div class='card'><div class='card-header'><h3>Water/Soil Probe</h3><span id='probeTemp' class='value-large'>--</span><span style='font-size:0.9rem'>°C</span></div><div><span id='probeStatus' class='badge badge-info'>--</span></div></div>";
  
  // Soil & Light
  html += "<div class='card'><div class='card-header'><h3>Soil Moisture</h3><span id='soil' class='value-large'>--</span><span style='font-size:0.9rem'>%</span></div><div class='progress'><div id='soilFill' class='progress-fill' style='width:0%'></div></div><div id='soilText' class='metric-label' style='margin-top:6px'>--</div></div>";
  
  html += "<div class='card'><div class='card-header'><h3>Light Level</h3><span id='light' class='value-large'>--</span><span style='font-size:0.9rem'>%</span></div><div class='progress'><div id='lightFill' class='progress-fill' style='width:0%;background:#f4a261'></div></div></div>";
  
  // Water & Rain
  html += "<div class='grid-2'>";
  html += "<div class='card'><div class='card-header'><h3>Water Tank</h3><span id='water' class='value-large'>--</span><span style='font-size:0.9rem'>%</span></div><div class='progress'><div id='waterFill' class='progress-fill' style='width:0%;background:#3498db'></div></div></div>";
  html += "<div class='card'><div class='card-header'><h3>Rain</h3><span id='rain' class='badge badge-success' style='font-size:0.9rem'>--</span></div></div>";
  html += "</div>";
  
  // Pump Status
  html += "<div class='card pump-card'><div class='card-header'><h3>Water Pump</h3><span id='pumpStatusDisplay' class='badge' style='font-size:0.8rem'>--</span></div><div class='pump-status' id='pumpIcon'>--</div><div id='pumpReason' class='metric-label' style='margin-top:8px'>--</div></div>";
  
  // Decision Logic
  html += "<div class='card'><div class='card-header'><h3>Decision Logic</h3></div>";
  html += "<div class='metric'><span class='metric-label'>🌱 Soil</span><span id='soilCondition' class='metric-value'>--</span></div>";
  html += "<div class='metric'><span class='metric-label'>💧 Tank</span><span id='tankCondition' class='metric-value'>--</span></div>";
  html += "<div class='metric'><span class='metric-label'>☔ Rain</span><span id='rainCondition' class='metric-value'>--</span></div>";
  html += "<div class='metric'><span class='metric-label'>🌡️ Probe</span><span id='tempCondition' class='metric-value'>--</span></div>";
  html += "</div>";
  
  html += "<div class='footer' id='lastUpdate'>--</div>";
  html += "</div>";
  
  // JavaScript
  html += "<script>";
  html += "async function fetchData(){";
  html += "try{let r=await fetch('/api/data');let d=await r.json();updateUI(d);}";
  html += "catch(e){console.log('Fetch error');}";
  html += "}";
  html += "function updateUI(d){";
  html += "document.getElementById('temp').innerHTML=d.temperature.toFixed(1);";
  html += "document.getElementById('hum').innerHTML=d.humidity.toFixed(0);";
  html += "if(d.probeTempValid){";
  html += "document.getElementById('probeTemp').innerHTML=d.probeTemp.toFixed(1);";
  html += "let hot=d.probeTemp>35;";
  html += "document.getElementById('probeStatus').innerHTML=hot?'⚠️ Elevated':'✓ Normal';";
  html += "document.getElementById('probeStatus').className=hot?'badge badge-warning':'badge badge-success';";
  html += "}else{";
  html += "document.getElementById('probeTemp').innerHTML='--';";
  html += "document.getElementById('probeStatus').innerHTML='❌ Disconnected';";
  html += "document.getElementById('probeStatus').className='badge badge-danger';";
  html += "}";
  html += "document.getElementById('soil').innerHTML=d.soilMoisture;";
  html += "document.getElementById('soilFill').style.width=d.soilMoisture+'%';";
  html += "let soilText=d.soilMoisture<30?'🌵 Dry — needs water':d.soilMoisture<60?'🌿 Moderate':'💧 Moist';";
  html += "document.getElementById('soilText').innerHTML=soilText;";
  html += "document.getElementById('light').innerHTML=d.light;";
  html += "document.getElementById('lightFill').style.width=d.light+'%';";
  html += "document.getElementById('water').innerHTML=d.waterLevel;";
  html += "document.getElementById('waterFill').style.width=d.waterLevel+'%';";
  html += "let rainText=d.rain?'🌧️ Detected':'☀️ Clear';";
  html += "let rainClass=d.rain?'badge badge-warning':'badge badge-success';";
  html += "document.getElementById('rain').innerHTML=rainText;";
  html += "document.getElementById('rain').className=rainClass;";
  
  // Decision logic
  html += "let soilDry=d.soilMoisture<30;";
  html += "let tankOk=d.waterLevelRaw>500;";
  html += "let raining=d.rain;";
  html += "let hot=(d.probeTempValid && d.probeTemp>35);";
  html += "let shouldRun=soilDry && tankOk && !raining && !hot;";
  
  html += "document.getElementById('soilCondition').innerHTML=soilDry?'✅ Dry (needs water)':'❌ Wet';";
  html += "document.getElementById('tankCondition').innerHTML=tankOk?'✅ Has water':'❌ Empty';";
  html += "document.getElementById('rainCondition').innerHTML=raining?'❌ Raining':'✅ Clear';";
  html += "document.getElementById('tempCondition').innerHTML=hot?'❌ Too hot':'✅ Normal';";
  
  // Pump display
  html += "let pumpOn=d.pumpOn;";
  html += "let pumpStatusText=pumpOn?'🟢 Running':'⚪ Off';";
  html += "let pumpClass=pumpOn?'badge badge-success':'badge badge-info';";
  html += "document.getElementById('pumpStatusDisplay').innerHTML=pumpStatusText;";
  html += "document.getElementById('pumpStatusDisplay').className=pumpClass;";
  html += "document.getElementById('pumpIcon').innerHTML=pumpOn?'💧 Pump Active':'⏸️ Pump Idle';";
  
  html += "let reason='';";
  html += "if(shouldRun){";
  html += "reason='✓ Soil dry + tank full + no rain → Pump ON';";
  html += "}else if(!soilDry){";
  html += "reason='→ Soil moisture adequate';";
  html += "}else if(!tankOk){";
  html += "reason='→ Tank empty — refill needed';";
  html += "}else if(raining){";
  html += "reason='→ Rain detected — using natural water';";
  html += "}else if(hot){";
  html += "reason='→ Water too hot — protecting plants';";
  html += "}";
  html += "document.getElementById('pumpReason').innerHTML=reason;";
  
  html += "document.getElementById('lastUpdate').innerHTML='Updated '+new Date().toLocaleTimeString();";
  html += "}";
  html += "fetchData();setInterval(fetchData,3000);";
  html += "</script></body></html>";
  
  return html;
}

// ==================== WEB HANDLERS ====================
void handleRoot() {
  server.send(200, "text/html", buildPage());
}

void handleApiData() {
  StaticJsonDocument<384> doc;
  doc["temperature"] = data.temperature;
  doc["humidity"] = data.humidity;
  doc["probeTemp"] = data.probeTempValid ? data.probeTemp : 0.0;
  doc["probeTempValid"] = data.probeTempValid;
  doc["rain"] = data.rainDetected;
  doc["waterLevel"] = data.waterLevelPct;
  doc["waterLevelRaw"] = data.waterLevelRaw;
  doc["light"] = data.lightPct;
  doc["soilMoisture"] = data.soilMoisturePct;
  doc["soilRaw"] = data.soilRaw;
  doc["pumpOn"] = data.pumpOn;
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n========================================");
  Serial.println("  Jigyasu — Learning by Doing");
  Serial.println("  Smart Irrigation System");
  Serial.println("========================================");

  // Initialize pins
  pinMode(RAIN_DO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  setPump(false);

  // Initialize OLED
  Wire.begin(21, 22);
  u8g2.begin();
  u8g2.setContrast(200);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x14B_tf);
  u8g2.drawStr(6, 22, "Jigyasu");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(20, 38, "Learning by Doing");
  u8g2.drawStr(14, 52, "Smart Irrigation");
  u8g2.sendBuffer();

  // Initialize sensors
  dht.begin();
  ds18b20.begin();
  ds18b20.setResolution(12);

  Serial.print("DS18B20 devices: ");
  Serial.println(ds18b20.getDeviceCount());

  delay(1000);

  // Setup WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("WiFi AP: ");
  Serial.println(AP_SSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(4, 18, "WiFi Ready");
  u8g2.drawStr(4, 32, AP_SSID);
  u8g2.drawStr(4, 46, "192.168.4.1");
  u8g2.drawStr(4, 60, "jigyasu.local");
  u8g2.sendBuffer();
  delay(2000);

  // Setup web server
  server.on("/", handleRoot);
  server.on("/api/data", handleApiData);
  server.onNotFound(handleNotFound);
  server.begin();

  readSensors();

  Serial.println("========================================");
  Serial.println("  SYSTEM READY");
  Serial.println("========================================");
  Serial.println("WiFi: JigyasuIrrigation");
  Serial.println("Pass: jigyasu123");
  Serial.println("Web: http://192.168.4.1");
  Serial.println("========================================\n");
}

// ==================== LOOP ====================
void loop() {
  server.handleClient();

  if (millis() - lastSensorRead > SENSOR_INTERVAL_MS) {
    readSensors();
    autoIrrigate();
    lastSensorRead = millis();
  }

  updateOLED();
}