/***************************************************************************
  This sketch reads BME688 sensor data, displays it on a Nokia 5110 LCD,
  and hosts a web page to display real-time data and historical plots.

  Version 3.5.3 Changelog:
  - FIXED WEB SERIAL FORMATTING: Changed the log element in the WebSerial
    HTML page from a <div> to a <pre> tag. This correctly renders newline
    characters, making the output readable like a standard serial monitor.
  - MAJOR NETWORKING REFACTOR: Replaced ESPAsyncWebServer with the standard
    WebServer and a dedicated WebSocketsServer library. This resolves
    persistent stability issues with the WebSerial connection.
  - CUSTOM WEBSERIAL: Replaced the WebSerial library with a lightweight,
    custom implementation compatible with the new server setup. The WebSerial
    page now connects to port 81.
  - ADDED OTA UPDATES: Integrated the ArduinoOTA library to allow for
    wireless firmware updates over WiFi.
  - MODIFIED STARTUP SEQUENCE: The device now performs a gas reading immediately
    on startup, followed by a 5-minute cool-down.
  - FIXED GAS SENSOR READING: Added a check for 'bme.gas_resistance > 0' after
    a gas measurement to prevent invalid 0.0 KOhm readings.
  - MEMORY OPTIMIZATION: Removed the large PSRAM history array for the gas
    sensor.
  - LCD UI UPDATE: Replaced the gas sensor plot on Page 5 with a dedicated
    page showing the last valid reading and the time it was recorded.
  - WEB UI ENHANCEMENT: Removed the gas sensor chart from the web page and
    added a "Last Updated" timestamp to the gas data card for clarity.
  - CRITICAL BUG FIX: Reinforced the historical data index calculation in the
    web server to prevent out-of-bounds memory access.
  - OPTIMIZED FLASH WRITES: Added a "Save" button to the web UI for RGB control.
  - ADJUSTED ENCODER RESOLUTION: Aligned encoder count to one "click" per value.
  - FIXED ROTARY SWITCH: Implemented robust state-change debounce logic.

  *** IMPORTANT ARDUINO IDE SETTING ***
  You MUST enable PSRAM in Tools -> PSRAM -> "OPI PSRAM" for this code to work.

  Hardware Connections for ESP32-S3:
  - BME688 Sensor (I2C): VCC(14), GND, SDA(8), SCL(9)
  - Nokia 5110 LCD (SPI): CLK(12), DIN(11), DC(6), CE(7), RST(15), BL(13)
  - Rotary Encoder (KY-040): CLK(1), DT(2), SW(3)
  - Onboard RGB LED: GPIO 48

  Library Dependencies:
  1. Adafruit BME680, Adafruit Unified Sensor
  2. Preferences (built-in)
  3. Adafruit GFX, Adafruit PCD8544, Adafruit_NeoPixel
  4. ESP32Encoder
  5. WiFi, WebServer, Arduino_JSON, ArduinoOTA, ESPmDNS
  6. WebSockets (by Markus Sattler, install from Library Manager)
***************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_BME680.h"
#include <Preferences.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_PCD8544.h>
#include <ESP32Encoder.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Arduino_JSON.h>
#include "driver/ledc.h"
#include "time.h"
#include <Adafruit_NeoPixel.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

// --- WiFi Configuration ---
const char* ssid = "Your_Wireless_SSID";
const char* password = "Your_Wireless_SSID_Password";

// --- NTP Configuration ---
const char* ntpServer = "asia.pool.ntp.org";
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;

// --- Pin Configuration ---
#define BME_POWER_PIN 14
#define ROTARY_ENCODER_A_PIN 1
#define ROTARY_ENCODER_B_PIN 2
#define ROTARY_ENCODER_BUTTON_PIN 3
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9
#define NEOPIXEL_PIN 48

// --- Sensor Configuration ---
#define BME688_I2C_ADDR 0x77

// --- Hardware SPI Pin Configuration (Nokia 5110) ---
Adafruit_PCD8544 display = Adafruit_PCD8544(6, 7, 15);

// --- Global Objects ---
Adafruit_BME680 bme;
ESP32Encoder rotaryEncoder;
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
Adafruit_NeoPixel pixels(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
Preferences preferences;

// --- Data Structure for Timestamped Readings ---
struct SensorReading {
  time_t timestamp;
  float value;
};

// --- Data History Configuration ---
const int HISTORY_SIZE = 2000;
const int CHUNK_SIZE = 200;
SensorReading *tempHistory, *humiHistory, *presHistory; // Gas history removed
int historyIndex = 0;
float lastValidGasReading = 0.0;
time_t lastGasReadTimestamp = 0; // For web UI and LCD

// --- Menu State ---
int currentPage = 0;
const int NUM_PAGES = 7;
bool needsRedraw = true;
long oldEncoderPosition = 0;

// --- Brightness Control State ---
int lcdBrightness = 128;
bool brightnessEditMode = false;

// --- RGB LED Control State ---
uint8_t redVal = 10, greenVal = 0, blueVal = 10;
enum RgbSelectState { SELECT_R, SELECT_G, SELECT_B };
RgbSelectState currentRgbSelectState = SELECT_R;
bool rgbEditMode = false;

// --- Sensor State Machine ---
enum SensorState { NORMAL_READING, GAS_MEASUREMENT, COOLING_DOWN };
SensorState currentSensorState = GAS_MEASUREMENT; // Start with a gas reading
unsigned long sensorReadingEndTime = 0;

// --- Timers ---
unsigned long lastGasReadTime = 0, coolingStartTime = 0, lastNtpSyncTime = 0;
const unsigned long GAS_READ_INTERVAL = 3600000; // 1 hour
const unsigned long COOL_DOWN_PERIOD = 300000;   // 5 minutes
const unsigned long NTP_SYNC_INTERVAL = 86400000; // 24 hours

// --- Button Debounce State ---
int buttonState = HIGH;
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const long debounceDelay = 50;

// --- Forward Declarations ---
void drawMainPage();
void drawPlotPage(SensorReading data[], const char* title, const char* unit, int decPlaces);
void drawSettingsPage();
void drawRgbPage();
void drawGasPage(); // New function for gas info
void handleSerialInput();
void handleRoot();
void handleData();
void handleLatestData();
void handleRgbSet();
void handleRgbSave();
void handleWebSerialPage();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
void webSerialPrint(String message);
void webSerialPrintln(String message);
void handleEncoder();
void syncTimeWithNTP();
void loadSettings();
void saveBrightness();
void saveRgbColor();
void handleSensorReadings();
void setupOTA();


void setup() {
  Serial.begin(115200);
  Serial.println(F("Weather Station Web Server - V3.5.3 Start"));

  // --- Allocate History Arrays in PSRAM ---
  Serial.println("Allocating history arrays in PSRAM...");
  tempHistory = (SensorReading*) ps_malloc(HISTORY_SIZE * sizeof(SensorReading));
  humiHistory = (SensorReading*) ps_malloc(HISTORY_SIZE * sizeof(SensorReading));
  presHistory = (SensorReading*) ps_malloc(HISTORY_SIZE * sizeof(SensorReading));

  if (!tempHistory || !humiHistory || !presHistory) {
    Serial.println("FATAL: Failed to allocate memory in PSRAM!");
    while(1);
  }
  Serial.println("PSRAM allocation successful.");

  // --- Load Saved Settings ---
  loadSettings();

  // --- Power on the BME688 Sensor ---
  pinMode(BME_POWER_PIN, OUTPUT);
  digitalWrite(BME_POWER_PIN, HIGH);
  delay(10);

  // --- WiFi Connection ---
  Serial.printf("Connecting to %s ", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" CONNECTED!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // --- Initialize NTP Time Client ---
  syncTimeWithNTP();

  // --- Over-the-Air (OTA) Update Setup ---
  setupOTA();

  // --- Web Server Setup ---
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/data/latest", handleLatestData);
  server.on("/rgb/set", handleRgbSet);
  server.on("/rgb/save", handleRgbSave);
  server.on("/webserial", handleWebSerialPage);
  server.begin();
  Serial.println("HTTP server started");
  
  // --- WebSocket (WebSerial) Server Setup ---
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("WebSocket server started on port 81");


  // --- Initialize BME688 Sensor ---
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!bme.begin(BME688_I2C_ADDR, &Wire)) {
    Serial.println(F("Could not find BME688 sensor!"));
    webSerialPrintln(F("Could not find BME688 sensor!"));
    while (1);
  }
  bme.setTemperatureOversampling(BME680_OS_16X);
  bme.setHumidityOversampling(BME680_OS_16X);
  bme.setPressureOversampling(BME680_OS_16X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150); // Heater ON for initial gas reading
  Serial.println("BME688 initialized. Starting with gas measurement.");
  webSerialPrintln("BME688 initialized. Starting with gas measurement.");

  // --- Initialize Rotary Encoder ---
  pinMode(ROTARY_ENCODER_BUTTON_PIN, INPUT_PULLUP);
  // The following line is not needed for the user's hardware module.
  // ESP32Encoder::useInternalWeakPullResistors = ESP32Encoder::UP;
  rotaryEncoder.attachFullQuad(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN);
  rotaryEncoder.setCount(0);
  oldEncoderPosition = 0;

  // --- Initialize Onboard RGB LED (with loaded settings) ---
  pixels.begin();
  pixels.setPixelColor(0, pixels.Color(redVal, greenVal, blueVal));
  pixels.show();

  // --- Initialize Display (with loaded settings) ---
  display.begin();
  
  const int backlightPin = 13;
  const int pwmFrequency = 5000;
  ledc_timer_config_t ledc_timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT, .timer_num = LEDC_TIMER_0, .freq_hz = pwmFrequency, .clk_cfg = LEDC_AUTO_CLK};
  ledc_timer_config(&ledc_timer);
  ledc_channel_config_t ledc_channel = {.gpio_num = backlightPin, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .intr_type = LEDC_INTR_DISABLE, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
  ledc_channel_config(&ledc_channel);
  uint32_t duty = 256 - lcdBrightness;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

  display.setContrast(55);
  display.setRotation(2);
  display.setTextSize(1);
  display.setTextColor(BLACK);

  // --- Initialize Data Arrays ---
  time_t now = time(nullptr);
  for (int i = 0; i < HISTORY_SIZE; i++) {
    tempHistory[i] = {now, 0}; humiHistory[i] = {now, 0}; presHistory[i] = {now, 0};
  }
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  webSocket.loop();

  if (millis() - lastNtpSyncTime > NTP_SYNC_INTERVAL) {
    syncTimeWithNTP();
  }

  handleSensorReadings();
  handleEncoder();
  handleSerialInput();

  if (needsRedraw) {
    display.clearDisplay();
    switch (currentPage) {
      case 0: drawMainPage();     break;
      case 1: drawPlotPage(tempHistory, "Temp", "C", 1);    break;
      case 2: drawPlotPage(humiHistory, "Humi", "%", 1);    break;
      case 3: drawPlotPage(presHistory, "Pres", "hPa", 1);  break;
      case 4: drawGasPage();      break;
      case 5: drawSettingsPage(); break;
      case 6: drawRgbPage();      break;
    }
    display.display();
    needsRedraw = false;
  }
}

// --- Sensor Logic ---
void handleSensorReadings() {
  unsigned long now = millis();

  // State transitions
  if (currentSensorState == COOLING_DOWN && now - coolingStartTime > COOL_DOWN_PERIOD) {
    Serial.println("Cool-down complete. Resuming normal readings.");
    webSerialPrintln("Cool-down complete. Resuming normal readings.");
    currentSensorState = NORMAL_READING;
  } else if (currentSensorState == NORMAL_READING && now - lastGasReadTime > GAS_READ_INTERVAL) {
    Serial.println("One hour passed. Starting gas measurement cycle.");
    webSerialPrintln("One hour passed. Starting gas measurement cycle.");
    bme.setGasHeater(320, 150);
    currentSensorState = GAS_MEASUREMENT;
    sensorReadingEndTime = 0; // Force a new reading to start
  }

  // Non-blocking read logic
  if (sensorReadingEndTime == 0 && currentSensorState != COOLING_DOWN) {
    sensorReadingEndTime = bme.beginReading();
  } else if (sensorReadingEndTime > 0 && now >= sensorReadingEndTime) {
    if (bme.endReading()) {
      time_t currentTime = time(nullptr);
      tempHistory[historyIndex] = {currentTime, bme.temperature};
      humiHistory[historyIndex] = {currentTime, bme.humidity};
      presHistory[historyIndex] = {currentTime, bme.pressure / 100.0f};

      if (currentSensorState == GAS_MEASUREMENT) {
        if (bme.gas_resistance > 0) {
          lastValidGasReading = bme.gas_resistance / 1000.0f;
          lastGasReadTimestamp = currentTime;
          Serial.print("Gas measurement successful: ");
          Serial.println(lastValidGasReading);
          webSerialPrint("Gas measurement successful: ");
          webSerialPrintln(String(lastValidGasReading));
        } else {
          Serial.println("Gas measurement failed: Invalid reading (0).");
          webSerialPrintln("Gas measurement failed: Invalid reading (0).");
        }
        
        Serial.println("Entering cool-down.");
        webSerialPrintln("Entering cool-down.");
        bme.setGasHeater(0, 0); // Turn heater off
        currentSensorState = COOLING_DOWN;
        coolingStartTime = now;
        lastGasReadTime = now;
      }
      
      historyIndex = (historyIndex + 1) % HISTORY_SIZE;
      needsRedraw = true;
    }
    sensorReadingEndTime = 0; // Reset for the next reading
  }
}

// --- Web Server Handler Functions ---

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 Weather Station</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns/dist/chartjs-adapter-date-fns.bundle.min.js"></script>
  <style>
    body { font-family: sans-serif; background-color: #f0f2f5; color: #333; }
    .container { max-width: 1000px; margin: 0 auto; padding: 15px; }
    .header { text-align: center; margin-bottom: 20px; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; }
    .card { background-color: white; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); padding: 20px; }
    .card h2 { margin-top: 0; font-size: 1.2em; color: #555; }
    .value { font-size: 2.5em; font-weight: bold; color: #007bff; }
    .chart-container, .rgb-container { margin-top: 30px; }
    .chart-header { display: flex; justify-content: space-between; align-items: center; }
    .nav-btn { background-color: #007bff; color: white; border: none; padding: 8px 12px; border-radius: 4px; cursor: pointer; }
    .nav-btn:disabled { background-color: #cccccc; }
    .slider-group { display: flex; align-items: center; margin-bottom: 10px; }
    .slider-group label { width: 60px; }
    .slider-group input { flex-grow: 1; }
    .timestamp { font-size: 0.8em; color: #666; margin-top: -10px; }
  </style>
</head>
<body>
  <div class="container">
    <div class="header"><h1>ESP32 Weather Station</h1></div>
    <div class="grid">
      <div class="card"><h2>Temperature</h2><p class="value"><span id="temp">--</span> &deg;C</p></div>
      <div class="card"><h2>Humidity</h2><p class="value"><span id="humi">--</span> %</p></div>
      <div class="card"><h2>Pressure</h2><p class="value"><span id="pres">--</span> hPa</p></div>
      <div class="card">
        <h2>Gas Resistance</h2>
        <p class="value"><span id="gas">--</span> k&Omega;</p>
        <p class="timestamp">Last updated: <span id="gas-updated">waiting...</span></p>
      </div>
    </div>
    <!-- Chart Containers -->
    <div class="chart-container card"><div class="chart-header"><button class="nav-btn" id="temp-prev">&lt;</button><h3>Temperature</h3><button class="nav-btn" id="temp-next">&gt;</button></div><canvas id="tempChart"></canvas></div>
    <div class="chart-container card"><div class="chart-header"><button class="nav-btn" id="humi-prev">&lt;</button><h3>Humidity</h3><button class="nav-btn" id="humi-next">&gt;</button></div><canvas id="humiChart"></canvas></div>
    <div class="chart-container card"><div class="chart-header"><button class="nav-btn" id="pres-prev">&lt;</button><h3>Pressure</h3><button class="nav-btn" id="pres-next">&gt;</button></div><canvas id="presChart"></canvas></div>
    
    <!-- RGB LED Control -->
    <div class="rgb-container card">
      <h2>Onboard LED Control</h2>
      <div class="slider-group"><label for="red">Red</label><input type="range" id="red" min="0" max="255" value="10"></div>
      <div class="slider-group"><label for="green">Green</label><input type="range" id="green" min="0" max="255" value="0"></div>
      <div class="slider-group"><label for="blue">Blue</label><input type="range" id="blue" min="0" max="255" value="10"></div>
      <button id="saveRgbBtn" class="nav-btn" style="margin-top: 10px;">Save Color</button>
    </div>
  </div>

  <script>
    const HISTORY_SIZE = 2000;
    const CHUNK_SIZE = 200;
    let currentOffset = 0;

    const chartConfig = (yAxisLabel) => ({
      type: 'line',
      data: { datasets: [{ label: yAxisLabel, data: [], borderColor: 'rgba(0, 123, 255, 0.8)', borderWidth: 2, pointRadius: 0, tension: 0.1 }] },
      options: { animation: false, scales: { x: { type: 'time', time: { tooltipFormat: 'PPpp' }, title: { display: true, text: 'Time' }, ticks: { maxTicksLimit: 6 } }, y: { beginAtZero: false, title: { display: true, text: yAxisLabel } } } }
    });

    const tempChart = new Chart(document.getElementById('tempChart'), chartConfig('Temperature (°C)'));
    const humiChart = new Chart(document.getElementById('humiChart'), chartConfig('Humidity (%)'));
    const presChart = new Chart(document.getElementById('presChart'), chartConfig('Pressure (hPa)'));

    function updateChart(chart, data) {
      const values = data.map(d => d.y);
      const dataMax = Math.max(...values);
      const dataMin = Math.min(...values);
      const padding = (dataMax - dataMin) * 0.1 || 1; 
      chart.options.scales.y.min = dataMin - padding;
      chart.options.scales.y.max = dataMax + padding;
      chart.data.datasets[0].data = data;
      chart.update();
    }
    
    function updateNavButtons() {
        document.querySelectorAll('.nav-btn').forEach(btn => {
            if (btn.id.includes('-prev')) btn.disabled = (currentOffset >= HISTORY_SIZE - CHUNK_SIZE);
            if (btn.id.includes('-next')) btn.disabled = (currentOffset <= 0);
        });
    }

    async function fetchData(offset = 0) {
      try {
        const response = await fetch(`/data?offset=${offset}`);
        const data = await response.json();
        
        updateChart(tempChart, data.temp);
        updateChart(humiChart, data.humi);
        updateChart(presChart, data.pres);
        updateNavButtons();
      } catch (error) { console.error('Error fetching data:', error); }
    }
    
    async function updateLatestData() {
      try {
        const response = await fetch('/data/latest');
        const data = await response.json();
        document.getElementById('temp').textContent = data.temp.toFixed(1);
        document.getElementById('humi').textContent = data.humi.toFixed(1);
        document.getElementById('pres').textContent = data.pres.toFixed(1);
        document.getElementById('gas').textContent = data.gas.toFixed(1);
        
        if (data.gas_updated > 0) {
            const gasUpdatedDate = new Date(data.gas_updated * 1000);
            document.getElementById('gas-updated').textContent = gasUpdatedDate.toLocaleString();
        } else {
            document.getElementById('gas-updated').textContent = 'waiting...';
        }
      } catch (error) {
        console.error('Error fetching latest data:', error);
      }
    }
    
    document.querySelectorAll('.nav-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            if (btn.id.includes('-prev')) {
                currentOffset = Math.min(currentOffset + CHUNK_SIZE, HISTORY_SIZE - CHUNK_SIZE);
            } else if (btn.id.includes('-next')) {
                currentOffset = Math.max(currentOffset - CHUNK_SIZE, 0);
            }
            fetchData(currentOffset);
        });
    });

    const redSlider = document.getElementById('red');
    const greenSlider = document.getElementById('green');
    const blueSlider = document.getElementById('blue');
    const saveRgbBtn = document.getElementById('saveRgbBtn');

    function setRgbData() {
        const r = redSlider.value;
        const g = greenSlider.value;
        const b = blueSlider.value;
        fetch(`/rgb/set?r=${r}&g=${g}&b=${b}`);
    }

    function saveRgbData() {
        fetch('/rgb/save');
    }

    redSlider.addEventListener('input', setRgbData);
    greenSlider.addEventListener('input', setRgbData);
    blueSlider.addEventListener('input', setRgbData);
    saveRgbBtn.addEventListener('click', saveRgbData);

    // Fast updates for cards
    setInterval(() => {
        if (currentOffset === 0) { updateLatestData(); }
    }, 5000); 

    // Slower updates for full chart
    setInterval(() => {
        if (currentOffset === 0) { fetchData(0); }
    }, 30000);

    fetchData(0); // Initial full load
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleData() {
  int offset = 0;
  if (server.hasArg("offset")) {
    offset = server.arg("offset").toInt();
  }
  offset = constrain(offset, 0, HISTORY_SIZE - 1);

  int start = (historyIndex - CHUNK_SIZE - offset);
  while (start < 0) {
    start += HISTORY_SIZE;
  }
  start %= HISTORY_SIZE;
  
  JSONVar jsonData;
  for (int i = 0; i < CHUNK_SIZE; i++) {
    int index = (start + i) % HISTORY_SIZE;
    jsonData["temp"][i]["x"] = (double)tempHistory[index].timestamp * 1000;
    jsonData["temp"][i]["y"] = tempHistory[index].value;
    jsonData["humi"][i]["x"] = (double)humiHistory[index].timestamp * 1000;
    jsonData["humi"][i]["y"] = humiHistory[index].value;
    jsonData["pres"][i]["x"] = (double)presHistory[index].timestamp * 1000;
    jsonData["pres"][i]["y"] = presHistory[index].value;
  }
  server.send(200, "application/json", JSON.stringify(jsonData));
}

void handleLatestData() {
  JSONVar jsonData;
  int currentIndex = (historyIndex == 0) ? HISTORY_SIZE - 1 : historyIndex - 1;
  
  jsonData["temp"] = tempHistory[currentIndex].value;
  jsonData["humi"] = humiHistory[currentIndex].value;
  jsonData["pres"] = presHistory[currentIndex].value;
  jsonData["gas"] = lastValidGasReading;
  jsonData["gas_updated"] = (double)lastGasReadTimestamp;
  
  server.send(200, "application/json", JSON.stringify(jsonData));
}

void handleRgbSet() {
  if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
    redVal = server.arg("r").toInt();
    greenVal = server.arg("g").toInt();
    blueVal = server.arg("b").toInt();
    
    pixels.setPixelColor(0, pixels.Color(redVal, greenVal, blueVal));
    pixels.show();
    
    needsRedraw = (currentPage == 6);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleRgbSave() {
  saveRgbColor();
  server.send(200, "text/plain", "Saved");
}

void handleWebSerialPage() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Web Serial</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: monospace; background-color: #1e1e1e; color: #d4d4d4; margin: 0; }
        #log { position: absolute; top: 0; bottom: 40px; left: 0; right: 0; overflow: auto; padding: 5px; white-space: pre-wrap; }
        #form { position: absolute; bottom: 0; left: 0; right: 0; height: 40px; display: flex; }
        #msg { flex-grow: 1; border: 1px solid #333; background-color: #252526; color: #d4d4d4; }
    </style>
</head>
<body>
    <pre id="log"></pre>
    <form id="form">
        <input type="text" id="msg" autocomplete="off"/>
        <button>Send</button>
    </form>
    <script>
        var gateway = `ws://${window.location.hostname}:81/`;
        var websocket;
        var log = document.getElementById('log');
        function initWebSocket() {
            console.log('Trying to open a WebSocket connection...');
            websocket = new WebSocket(gateway);
            websocket.onopen    = onOpen;
            websocket.onclose   = onClose;
            websocket.onmessage = onMessage;
        }
        function onOpen(event) {
            console.log('Connection opened');
            log.textContent += "Connected to ESP32\r\n";
        }
        function onClose(event) {
            console.log('Connection closed');
            log.textContent += "Connection Closed\r\n";
            setTimeout(initWebSocket, 2000);
        }
        function onMessage(event) {
            log.textContent += event.data;
            log.scrollTop = log.scrollHeight;
        }
        window.addEventListener('load', initWebSocket);
        document.getElementById('form').addEventListener('submit', function(e) {
            e.preventDefault();
            var msg = document.getElementById('msg');
            websocket.send(msg.value);
            msg.value = '';
        });
    </script>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      break;
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
      webSocket.sendTXT(num, "Connected to ESP32\r\n");
      }
      break;
    case WStype_TEXT:
      // This is where we handle incoming commands from the web page
      if (length > 0) {
        char input = payload[0];
        if (input >= '0' && input < '0' + NUM_PAGES) {
          int newPage = input - '0';
          if (newPage != currentPage) {
            currentPage = newPage;
            rotaryEncoder.setCount(currentPage * 4);
            oldEncoderPosition = currentPage;
            needsRedraw = true;
            Serial.printf("WebSerial: Switched to page: %d\n", currentPage);
            webSerialPrintln("Switched to page: " + String(currentPage));
          }
        }
      }
      break;
  }
}

void webSerialPrint(String message) {
  webSocket.broadcastTXT(message);
}

void webSerialPrintln(String message) {
  webSocket.broadcastTXT(message + "\r\n");
}


// --- Local UI and Helper Functions ---

void handleEncoder() {
    bool buttonClicked = false;
    int reading = digitalRead(ROTARY_ENCODER_BUTTON_PIN);

    // If the switch changed, due to noise or pressing
    if (reading != lastButtonState) {
        // reset the debouncing timer
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        // whatever the reading is at, it's been there for longer than the debounce
        // delay, so take it as the actual current state:

        // if the button state has changed:
        if (reading != buttonState) {
            buttonState = reading;

            // only trigger if the new button state is LOW (pressed)
            if (buttonState == LOW) {
                buttonClicked = true;
            }
        }
    }
    lastButtonState = reading;

    long newEncoderPosition = rotaryEncoder.getCount() / 4;
    bool encoderRotated = (newEncoderPosition != oldEncoderPosition);

    if (!buttonClicked && !encoderRotated) {
        return;
    }

    int previousPage = currentPage;

    if (encoderRotated && !(brightnessEditMode && currentPage == 5) && !(rgbEditMode && currentPage == 6)) {
        currentPage += (newEncoderPosition - oldEncoderPosition);
        currentPage = constrain(currentPage, 0, NUM_PAGES - 1);
    }

    if (currentPage == 5) {
        if (buttonClicked) {
            brightnessEditMode = !brightnessEditMode;
            if (brightnessEditMode) {
                rotaryEncoder.setCount(lcdBrightness * 4);
            } else {
                saveBrightness();
            }
        }
        if (encoderRotated && brightnessEditMode) {
            lcdBrightness = constrain(newEncoderPosition, 0, 255);
            uint32_t duty = 256 - lcdBrightness;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        }
    } else if (currentPage == 6) {
        if (buttonClicked) {
            if (!rgbEditMode) {
                rgbEditMode = true;
                currentRgbSelectState = SELECT_R;
                rotaryEncoder.setCount(redVal * 4);
            } else {
                if (currentRgbSelectState == SELECT_R) {
                    currentRgbSelectState = SELECT_G;
                    rotaryEncoder.setCount(greenVal * 4);
                } else if (currentRgbSelectState == SELECT_G) {
                    currentRgbSelectState = SELECT_B;
                    rotaryEncoder.setCount(blueVal * 4);
                } else {
                    rgbEditMode = false;
                    saveRgbColor();
                }
            }
        }
        if (encoderRotated && rgbEditMode) {
            int val = constrain(newEncoderPosition, 0, 255);
            if (currentRgbSelectState == SELECT_R) redVal = val;
            else if (currentRgbSelectState == SELECT_G) greenVal = val;
            else if (currentRgbSelectState == SELECT_B) blueVal = val;
            pixels.setPixelColor(0, pixels.Color(redVal, greenVal, blueVal));
            pixels.show();
        }
    }

    if (currentPage != previousPage) {
        brightnessEditMode = false;
        rgbEditMode = false;
    }

    oldEncoderPosition = newEncoderPosition;
    needsRedraw = true;
}


void handleSerialInput() {
  if (Serial.available() > 0) {
    char input = Serial.read();
    if (input >= '0' && input < '0' + NUM_PAGES) {
      int newPage = input - '0';
      if (newPage != currentPage) {
        currentPage = newPage;
        rotaryEncoder.setCount(currentPage * 4);
        oldEncoderPosition = currentPage;
        needsRedraw = true;
        Serial.printf("Switched to page: %d\n", currentPage);
        webSerialPrintln("Switched to page: " + String(currentPage));
      }
    }
    while(Serial.available() > 0) { Serial.read(); }
  }
}

void syncTimeWithNTP() {
  Serial.print("Synchronizing time with NTP server... ");
  webSerialPrint("Synchronizing time with NTP server... ");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    webSerialPrintln("Failed to obtain time");
  } else {
    Serial.println("Time synchronized");
    webSerialPrintln("Time synchronized");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
    // WebSerial doesn't support the same format specifiers, so we do it manually
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%A, %B %d %Y %H:%M:%S", &timeinfo);
    webSerialPrintln(timeStr);
  }
  lastNtpSyncTime = millis();
}

// --- Persistence Functions ---

void loadSettings() {
  preferences.begin("weather-stn", true);
  lcdBrightness = preferences.getInt("brightness", 128);
  redVal = preferences.getUChar("led_r", 10);
  greenVal = preferences.getUChar("led_g", 0);
  blueVal = preferences.getUChar("led_b", 10);
  preferences.end();
  Serial.println("Loaded settings from NVS.");
}

void saveBrightness() {
  preferences.begin("weather-stn", false);
  preferences.putInt("brightness", lcdBrightness);
  preferences.end();
  Serial.println("Saved brightness to NVS.");
  webSerialPrintln("Saved brightness to NVS.");
}

void saveRgbColor() {
  preferences.begin("weather-stn", false);
  preferences.putUChar("led_r", redVal);
  preferences.putUChar("led_g", greenVal);
  preferences.putUChar("led_b", blueVal);
  preferences.end();
  Serial.println("Saved RGB color to NVS.");
  webSerialPrintln("Saved RGB color to NVS.");
}


// --- Drawing Functions ---

void drawMainPage() {
  display.setCursor(0, 0);
  int currentIndex = (historyIndex == 0) ? HISTORY_SIZE - 1 : historyIndex - 1;
  display.print("Temp: "); display.print(tempHistory[currentIndex].value, 1); display.println("C");
  display.print("Humi: "); display.print(humiHistory[currentIndex].value, 1); display.println("%");
  display.print("Pres: "); display.print(presHistory[currentIndex].value, 1); display.println("hPa");
  display.print("Gas: ");  display.print(lastValidGasReading, 1); display.println("KOhm");
  
  time_t now = time(nullptr);
  struct tm * timeinfo = localtime(&now);
  char timeString[20];
  strftime(timeString, sizeof(timeString), "%d/%m %H:%M:%S", timeinfo);
  display.print(timeString);
  
  if (currentSensorState == GAS_MEASUREMENT) {
    display.setCursor(58, 40); display.print("HEAT");
  } else if (currentSensorState == COOLING_DOWN) {
    display.setCursor(58, 40); display.print("COOL");
  }
}

void drawPlotPage(SensorReading data[], const char* title, const char* unit, int decPlaces) {
  const int plotHeight = 32, plotYoffset = 16;
  int currentIndex = (historyIndex == 0) ? HISTORY_SIZE - 1 : historyIndex - 1;

  display.setCursor(0, 0);
  display.print(title); display.print(": "); display.print(data[currentIndex].value, decPlaces); display.print(unit);
  display.drawRect(0, plotYoffset, 84, plotHeight, BLACK);

  float minVal = 1.0e6, maxVal = -1.0e6;
  for (int i = 0; i < 82; i++) {
    int index = (historyIndex - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
    if (data[index].value < minVal) minVal = data[index].value;
    if (data[index].value > maxVal) maxVal = data[index].value;
  }
  if (maxVal == minVal) maxVal = minVal + 1;

  for (int x = 1; x < 83; x++) {
    int index = (historyIndex - 1 - (82 - x) + HISTORY_SIZE) % HISTORY_SIZE;
    int y = (plotHeight - 2) - ((data[index].value - minVal) / (maxVal - minVal) * (plotHeight - 3));
    y += (plotYoffset + 1);
    display.drawPixel(x, y, BLACK);
  }
}

void drawSettingsPage() {
  display.setCursor(0, 0);
  display.println("Settings");
  display.print("IP:");
  display.println(WiFi.localIP());
  display.print("Bright: ");
  display.print(lcdBrightness);
  if (brightnessEditMode) {
    display.print(" [EDIT]");
  }
}

void drawRgbPage() {
  display.setCursor(0, 0);
  display.println("RGB LED Control");
  
  const char* r_prefix = (rgbEditMode && currentRgbSelectState == SELECT_R) ? ">R: " : " R: ";
  const char* g_prefix = (rgbEditMode && currentRgbSelectState == SELECT_G) ? ">G: " : " G: ";
  const char* b_prefix = (rgbEditMode && currentRgbSelectState == SELECT_B) ? ">B: " : " B: ";

  display.setCursor(5, 15);
  display.print(r_prefix);
  display.print(redVal);

  display.setCursor(5, 25);
  display.print(g_prefix);
  display.print(greenVal);

  display.setCursor(5, 35);
  display.print(b_prefix);
  display.print(blueVal);
  
  if (!rgbEditMode) {
      display.setCursor(15, 40);
      display.print("(NAVIGATE)");
  } else {
      display.setCursor(20, 40);
      display.print("(EDITING)");
  }
}

void drawGasPage() {
  display.setCursor(0, 0);
  display.println("Gas Reading");

  display.setCursor(0, 15);
  display.print("Val: ");
  display.print(lastValidGasReading, 1);
  display.println(" KOhm");

  if (lastGasReadTimestamp > 0) {
    struct tm * timeinfo = localtime(&lastGasReadTimestamp);
    char timeString[20];
    strftime(timeString, sizeof(timeString), "%d/%m %H:%M:%S", timeinfo);
    display.setCursor(0, 30);
    display.println("Last Updated:");
    display.setCursor(0, 40);
    display.print(timeString);
  } else {
    display.setCursor(0, 30);
    display.print("Waiting for data...");
  }
}

// --- OTA Setup ---
void setupOTA() {
  ArduinoOTA.setHostname("esp32-weather-station");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    Serial.println("Start updating " + type);
    webSerialPrintln("Start updating " + type);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA Update");
    display.display();
    pixels.setPixelColor(0, pixels.Color(0, 0, 50)); // Blue
    pixels.show();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    webSerialPrintln("\nEnd");
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("Update Done!");
    display.println("Rebooting...");
    display.display();
    pixels.setPixelColor(0, pixels.Color(0, 50, 0)); // Green
    pixels.show();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int percentage = (progress / (float)total) * 100;
    Serial.printf("Progress: %u%%\r", percentage);
    webSerialPrintln("Progress: " + String(percentage) + "%");
    display.setCursor(0, 20);
    display.print("Progress: ");
    display.print(percentage);
    display.println("%");
    display.drawRect(0, 32, 84, 10, BLACK);
    display.fillRect(2, 34, (80 * percentage) / 100, 6, BLACK);
    display.display();
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    webSerialPrint("Error[" + String(error) + "]: ");
    if (error == OTA_AUTH_ERROR) { Serial.println("Auth Failed"); webSerialPrintln("Auth Failed"); }
    else if (error == OTA_BEGIN_ERROR) { Serial.println("Begin Failed"); webSerialPrintln("Begin Failed"); }
    else if (error == OTA_CONNECT_ERROR) { Serial.println("Connect Failed"); webSerialPrintln("Connect Failed"); }
    else if (error == OTA_RECEIVE_ERROR) { Serial.println("Receive Failed"); webSerialPrintln("Receive Failed"); }
    else if (error == OTA_END_ERROR) { Serial.println("End Failed"); webSerialPrintln("End Failed"); }
    
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("Update Failed!");
    display.display();
    pixels.setPixelColor(0, pixels.Color(50, 0, 0)); // Red
    pixels.show();
  });

  ArduinoOTA.begin();
}
