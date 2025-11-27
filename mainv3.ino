#include <WiFi.h>
#include <WebServer.h>
#include <RTClib.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>

// Hardware pins
#define RESET_PIN 0
#define LED_PIN 2
#define RELAY_PIN 33  // Relay (HS-F17P) signal pin on GPIO 33 (output-capable)
#define SERVO_PIN 32  // Servo for medicine dispenser on GPIO 32
#define BUZZER_PIN 25 // Buzzer signal pin on GPIO 25

// Initialize components
RTC_DS1307 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);
Servo myservo;  // Servo object using ESP32Servo

// Global variables
int nextID = 1;
int status = 0;
unsigned long lastDisplayUpdate = 0;
const unsigned long displayInterval = 1000;
bool alertActive = false;  // Alert state for flashing
unsigned long alertStartTime = 0;
const unsigned long alertDuration = 20000;  // 20 seconds alert
unsigned long lastNTPSync = 0;
const unsigned long ntpSyncInterval = 3600000;  // 1 hour in ms

DynamicJsonDocument doc(2048);
JsonArray eventDataArray = doc.to<JsonArray>();

// Log system
#define MAX_LOGS 20
String logs[MAX_LOGS];
int logCount = 0;

// Test alert state machine
enum TestState { TEST_IDLE, TEST_SERVO_START, TEST_SERVO_MID, TEST_SERVO_END, TEST_RELAY_HIGH, TEST_RELAY_WAIT };
TestState testState = TEST_IDLE;
unsigned long testTimer = 0;

void addLog(String message) {
  // Shift logs down
  for (int i = MAX_LOGS - 1; i > 0; i--) {
    logs[i] = logs[i - 1];
  }
  logs[0] = String(millis()) + " - " + message;
  
  if (logCount < MAX_LOGS) {
    logCount++;
  }
  
  Serial.println("[LOG] " + message);
}

// Simple HTML pages
const char* dashboardHTML = R"rawliteral(
<!DOCTYPE html>
<html><head><title>Medicine Dispenser</title>
<meta http-equiv="refresh" content="30">
<style>body{font-family:Arial;margin:20px;background:#f0f0f0;} 
.container{max-width:1000px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 0 10px rgba(0,0,0,0.1);}
table{border-collapse:collapse;width:100%;margin:20px 0;} th,td{border:1px solid #ddd;padding:12px;text-align:left;} 
th{background-color:#4CAF50;color:white;} .btn{display:inline-block;padding:10px 20px;background:#4CAF50;color:white;text-decoration:none;border-radius:5px;margin:5px;transition:background 0.3s;} 
.btn:hover{background:#45a049;} .status-bar{background:#e7f3ff;padding:15px;border-radius:5px;margin:15px 0;font-size:16px;line-height:1.6;}
.logs-container{margin-top:20px;} .logs-bar{background:#f5f5f5;border:1px solid #ddd;padding:10px;border-radius:5px;max-height:300px;overflow-y:auto;}
.log-entry{margin:5px 0;padding:5px;background:#fff;border-radius:3px;border-left:3px solid #2196F3;}
.log-entry:nth-child(even){background:#f9f9f9;}
.log-time{font-size:12px;color:#666;display:inline-block;width:80px;}
</style>
</head>
<body>
<div class="container">
<h1>Medicine Dispenser</h1>
<div class="status-bar">
  <strong>Status:</strong> <span id="status">Connected</span><br>
  <strong>IP:</strong> <span id="ip"></span><br>
  <strong>RTC Time:</strong> <span id="rtcTime"></span><br>
  <strong>Browser Time:</strong> <span id="browserTime"></span>
</div>

<a href="/addEvent" class="btn">Add Event</a>
<a href="/debug" class="btn" style="background:#2196F3;">Debug</a>
<a href="/testalert" class="btn" style="background:#FF9800;">Test Alert</a>
<a href="/clear" class="btn" style="background:#f44336;">Clear All Events</a>
<a href="/clearlogs" class="btn" style="background:#9C27B0;">Clear Logs</a>

<h2>Scheduled Events</h2>
<table><thead><tr><th>ID</th><th>Repeat</th><th>Time</th><th>Storage</th><th>Amount</th><th>Action</th></tr></thead><tbody id="events"></tbody></table>

<div class="logs-container">
<h2>Recent Activity</h2>
<div class="logs-bar" id="logs"></div>
</div>

<script>
document.getElementById('ip').innerText = location.hostname || 'Loading...';
setInterval(()=>{
  document.getElementById('browserTime').innerText = new Date().toLocaleString();
},1000);

function formatRelativeTime(ms) {
  let seconds = Math.floor(ms / 1000);
  if (seconds < 60) return seconds + 's ago';
  let minutes = Math.floor(seconds / 60);
  if (minutes < 60) return minutes + 'm ago';
  let hours = Math.floor(minutes / 60);
  return hours + 'h ago';
}

function loadEvents() {
  fetch('/events').then(r=>r.json()).then(events=>{
    let html = events.length ? '' : '<tr><td colspan="6">No events scheduled</td></tr>';
    events.forEach(e=>html += `<tr><td>${e.id}</td><td>${e.repeat}</td><td>${e.time}</td><td>${e.storageId}</td><td>${e.amount}</td><td><a href="/delete?id=${e.id}" class="btn" style="background:#f44336;font-size:12px;padding:5px 10px;">Delete</a></td></tr>`);
    document.getElementById('events').innerHTML = html;
  }).catch(e=>console.log('Events load failed:',e));
}

function loadLogs() {
  fetch('/logs').then(r=>r.text()).then(logs=>{
    let logArray = logs.split('\n').filter(line => line.trim() !== '');
    let html = '';
    let now = Date.now();
    logArray.slice(0, 10).forEach(log=>{
      let parts = log.split(' - ');
      if (parts.length >= 2) {
        let ms = parseInt(parts[0]);
        let time = formatRelativeTime(now - ms);
        let message = parts.slice(1).join(' - ');
        html += `<div class="log-entry"><span class="log-time">${time}</span> ${message}</div>`;
      }
    });
    document.getElementById('logs').innerHTML = html || '<div style="color:#999;text-align:center;padding:20px;">No recent activity</div>';
  }).catch(e=>console.log('Logs load failed:',e));
}

function loadRTCTime() {
  fetch('/rtctime').then(r=>r.text()).then(time=>{
    document.getElementById('rtcTime').innerText = time;
  }).catch(e=>console.log('RTC time load failed:',e));
}

loadEvents();
loadLogs();
loadRTCTime();
setInterval(loadEvents, 5000);
setInterval(loadLogs, 3000);
setInterval(loadRTCTime, 5000);
</script>
</body></html>
)rawliteral";

const char* addEventHTML = R"rawliteral(
<!DOCTYPE html>
<html><head><title>Add Event</title>
<style>body{font-family:Arial;margin:20px;background:#f0f0f0;} 
.container{max-width:500px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 0 10px rgba(0,0,0,0.1);}
input,select{padding:10px;margin:10px 0;width:100%;border:1px solid #ddd;border-radius:5px;box-sizing:border-box;}
.btn{display:block;width:100%;padding:12px;background:#4CAF50;color:white;border:none;border-radius:5px;margin:20px 0;cursor:pointer;transition:background 0.3s;text-align:center;box-sizing:border-box;}
.btn:hover{background:#45a049;}
.btn-back{background:#2196F3;}
.btn-back:hover{background:#1E88E5;}
.time-picker{text-align:center;margin:20px 0;}
.time-btn{padding:10px 20px;background:#ddd;color:#000;border:none;border-radius:5px;cursor:pointer;transition:background 0.3s;}
.time-btn:hover{background:#ccc;}
.time-display{font-size:24px;margin:0 10px;}
</style></head>
<body>
<div class="container">
<h1>Add Medicine Event</h1>
<form action="/submit" method="POST">
  <label><strong>Repeat Type:</strong></label>
  <select name="repeat" style="font-size:16px;">
    <option value="Daily">Daily</option>
    <option value="Weekly">Weekly</option>
    <option value="*2">Every 2 Days</option>
    <option value="*3">Every 3 Days</option>
    <option value="*7">Every Week</option>
  </select>
  
  <label><strong>Time (24hr):</strong></label>
  <div class="time-picker">
    <button type="button" class="time-btn" onclick="adjustHour(-1)">-</button>
    <span id="hour" class="time-display">12</span>
    <button type="button" class="time-btn" onclick="adjustHour(1)">+</button>
    :
    <button type="button" class="time-btn" onclick="adjustMin(-1)">-</button>
    <span id="min" class="time-display">00</span>
    <button type="button" class="time-btn" onclick="adjustMin(1)">+</button>
    <button type="button" class="time-btn" onclick="toggleAmPm()" style="margin-left:10px;">AM/PM</button>
    <span id="ampm" class="time-display">AM</span>
  </div>
  <input type="hidden" name="time" id="timeInput" value="00:00">
  
  <label><strong>Storage Slot (1-10):</strong></label>
  <select name="storageId">
    <option value="1">Storage 1</option>
    <option value="2">Storage 2</option>
    <option value="3">Storage 3</option>
    <option value="4">Storage 4</option>
    <option value="5">Storage 5</option>
    <option value="6">Storage 6</option>
    <option value="7">Storage 7</option>
    <option value="8">Storage 8</option>
    <option value="9">Storage 9</option>
    <option value="10">Storage 10</option>
  </select>
  
  <label><strong>Amount:</strong></label>
  <input type="number" name="amount" min="1" max="50" value="1" required>
  
  <button type="submit" class="btn">Save Event</button>
</form>
<a href="/" class="btn btn-back">Back to Dashboard</a>
</div>
<script>
let hour = 12;
let min = 0;
let ampm = 'AM';

function updateTimeInput() {
  let hh = hour;
  if (ampm === 'PM' && hour < 12) hh += 12;
  if (ampm === 'AM' && hour === 12) hh = 0;
  let timeStr = (hh < 10 ? '0' : '') + hh + ':' + (min < 10 ? '0' : '') + min;
  document.getElementById('timeInput').value = timeStr;
}

function adjustHour(delta) {
  hour += delta;
  if (hour > 12) hour = 1;
  if (hour < 1) hour = 12;
  document.getElementById('hour').innerText = hour;
  updateTimeInput();
}

function adjustMin(delta) {
  min += delta;
  if (min >= 60) min = 0;
  if (min < 0) min = 59;
  document.getElementById('min').innerText = (min < 10 ? '0' : '') + min;
  updateTimeInput();
}

function toggleAmPm() {
  ampm = ampm === 'AM' ? 'PM' : 'AM';
  document.getElementById('ampm').innerText = ampm;
  updateTimeInput();
}

updateTimeInput();
</script>
</body></html>
)rawliteral";

const char* debugHTML = R"rawliteral(
<!DOCTYPE html>
<html><head><title>Debug</title>
<style>body{font-family:Arial;margin:20px;background:#f0f0f0;} 
.container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 0 10px rgba(0,0,0,0.1);}
h2{margin-top:20px;} .info-box{background:#e7f3ff;padding:15px;border-radius:5px;margin-bottom:20px;}
.logs-container{margin-top:20px;} .logs-bar{background:#f5f5f5;border:1px solid #ddd;padding:10px;border-radius:5px;max-height:300px;overflow-y:auto;}
.log-entry{margin:5px 0;padding:5px;background:#fff;border-radius:3px;border-left:3px solid #2196F3;}
.log-entry:nth-child(even){background:#f9f9f9;}
.log-time{font-size:12px;color:#666;display:inline-block;width:80px;}
.btn{display:block;width:100%;padding:12px;border:none;border-radius:5px;margin:10px 0;cursor:pointer;transition:background 0.3s;text-decoration:none;text-align:center;box-sizing:border-box;}
.btn-debug{background:#2196F3;color:white;}
.btn-debug:hover{background:#1E88E5;}
.btn-clear{background:#9C27B0;color:white;}
.btn-clear:hover{background:#7B1FA2;}
.btn-restart{background:#f44336;color:white;}
.btn-restart:hover{background:#d32f2f;}
</style></head>
<body>
<div class="container">
<h1>Debug Information</h1>
<div class="info-box">
  <h2>System Status</h2>
  <p><strong>RTC Time:</strong> <span id="rtcTime"></span></p>
  <p><strong>WiFi Status:</strong> <span id="wifiStatus"></span></p>
  <p><strong>WiFi Signal:</strong> <span id="wifiSignal"></span> dBm</p>
  <p><strong>IP Address:</strong> <span id="ip"></span></p>
  <p><strong>Free Heap:</strong> <span id="freeHeap"></span> bytes</p>
  <p><strong>Event Count:</strong> <span id="eventCount"></span></p>
  <p><strong>Uptime:</strong> <span id="uptime"></span></p>
</div>
<a href="/testlcd" class="btn btn-debug">Test LCD</a>
<a href="/testservo" class="btn btn-debug">Test Servo</a>
<a href="/testrelay" class="btn btn-debug">Test Relay</a>
<a href="/restart" class="btn btn-restart" onclick="return confirm('Are you sure you want to restart the ESP32?');">Restart ESP32</a>
<a href="/clearlogs" class="btn btn-clear" onclick="return confirm('Are you sure you want to clear all logs?');">Clear Logs</a>
<div class="logs-container">
  <h2>System Logs</h2>
  <div class="logs-bar" id="logs"></div>
</div>
<a href="/" class="btn btn-debug">Back to Dashboard</a>
</div>
<script>
document.getElementById('ip').innerText = location.hostname || 'Loading...';
function formatRelativeTime(ms) {
  let seconds = Math.floor(ms / 1000);
  if (seconds < 60) return seconds + 's ago';
  let minutes = Math.floor(seconds / 60);
  if (minutes < 60) return minutes + 'm ago';
  let hours = Math.floor(minutes / 60);
  return hours + 'h ago';
}
fetch('/debuginfo').then(r=>r.json()).then(data=>{
  document.getElementById('rtcTime').innerText = data.rtcTime;
  document.getElementById('wifiStatus').innerText = data.wifiStatus;
  document.getElementById('wifiSignal').innerText = data.wifiSignal;
  document.getElementById('freeHeap').innerText = data.freeHeap;
  document.getElementById('eventCount').innerText = data.eventCount;
  document.getElementById('uptime').innerText = data.uptime;
}).catch(e=>console.log('Debug info load failed:',e));
fetch('/logs').then(r=>r.text()).then(logs=>{
  let logArray = logs.split('\n').filter(line => line.trim() !== '');
  let html = '';
  let now = Date.now();
  logArray.forEach(log=>{
    let parts = log.split(' - ');
    if (parts.length >= 2) {
      let ms = parseInt(parts[0]);
      let time = formatRelativeTime(now - ms);
      let message = parts.slice(1).join(' - ');
      html += `<div class="log-entry"><span class="log-time">${time}</span> ${message}</div>`;
    }
  });
  document.getElementById('logs').innerHTML = html || '<div style="color:#999;text-align:center;padding:20px;">No recent activity</div>';
}).catch(e=>console.log('Logs load failed:',e));
</script>
</body></html>
)rawliteral";

const char* successHTMLTemplate = R"rawliteral(
<!DOCTYPE html>
<html><head><title>%TITLE%</title>
<style>body{font-family:Arial;margin:20px;background:#f0f0f0;} 
.container{max-width:500px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 0 10px rgba(0,0,0,0.1);text-align:center;}
h1{color:#4CAF50;} .btn{display:block;width:100%;padding:12px;background:#2196F3;color:white;border:none;border-radius:5px;margin:20px 0;cursor:pointer;transition:background 0.3s;text-decoration:none;text-align:center;box-sizing:border-box;}
.btn:hover{background:#1E88E5;} .message{margin:20px 0;padding:15px;background:#e7f3ff;border-radius:5px;}
</style></head>
<body>
<div class="container">
<h1>%TITLE%</h1>
<div class="message">%MESSAGE%</div>
<a href="/" class="btn">Back to Dashboard</a>
</div>
</body></html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Medicine Dispenser Starting ===");
  
  // Initialize log system
  addLog("System started");
  
  // Initialize hardware
  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);  // Relay off (assuming active-high)
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // Buzzer off
  
  myservo.attach(SERVO_PIN, 500, 2400);  // Attach servo with min/max pulse width
  myservo.write(0);  // Initialize servo to 0 degrees
  addLog("Servo initialized on GPIO " + String(SERVO_PIN));
  
  // I2C and LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Booting...");
  addLog("LCD ready");
  
  // WiFi Manager
  WiFiManager wifiManager;
  wifiManager.setTimeout(180);  // 3 minutes timeout
  
  // Reset if button pressed
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("Reset button pressed - Clearing WiFi");
    addLog("WiFi reset");
    wifiManager.resetSettings();
    lcd.clear();
    lcd.print("WiFi Reset");
    delay(2000);
  }
  
  // Connect to WiFi
  Serial.println("Connecting to WiFi...");
  addLog("Connecting WiFi");
  lcd.clear();
  lcd.print("WiFi...");
  
  bool connected = wifiManager.autoConnect("MedicineDispenserAP");
  
  if (!connected) {
    Serial.println("WiFi connection failed!");
    addLog("WiFi failed");
    lcd.clear();
    lcd.print("WiFi Failed");
    lcd.setCursor(0, 1);
    lcd.print("Restarting...");
    delay(3000);
    ESP.restart();
  }
  
  // Success!
  Serial.println("WiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
  addLog("WiFi connected: " + WiFi.localIP().toString());
  
  // Update LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("IP: ");
  lcd.print(WiFi.localIP());
  lcd.setCursor(0, 1);
  lcd.print("Ready!");
  delay(2000);
  
  // Initialize RTC
  if (!rtc.begin()) {
    Serial.println("RTC not found - using system time");
    addLog("No RTC found");
    lcd.clear();
    lcd.print("No RTC");
    delay(2000);
  } else {
    Serial.println("RTC initialized");
    addLog("RTC ready");
    if (!rtc.isrunning()) {
      Serial.println("Setting RTC to compile time");
      addLog("RTC set to compile time");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    
    // Display current RTC time on startup
    DateTime now = rtc.now();
    Serial.println("Current RTC time: " + now.timestamp());
    addLog("RTC time: " + now.timestamp());
  }
  
  // Sync RTC with NTP after WiFi connection
  syncRTCWithNTP();
  lastNTPSync = millis();
  
  // Retry NTP sync if RTC time is invalid (e.g., pre-2020 or post-2038)
  DateTime now = rtc.now();
  if (now.year() < 2020 || now.year() > 2038) {
    addLog("Invalid RTC time detected, retrying NTP");
    syncRTCWithNTP();
  }
  
  // Setup web server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/addEvent", HTTP_GET, handleAddEvent);
  server.on("/submit", HTTP_POST, handleSubmit);
  server.on("/events", HTTP_GET, handleGetEvents);
  server.on("/delete", HTTP_GET, handleDeleteEvent);
  server.on("/clear", HTTP_GET, handleClearEvents);
  server.on("/debug", HTTP_GET, handleDebug);
  server.on("/debuginfo", HTTP_GET, handleDebugInfo);
  server.on("/testalert", HTTP_GET, handleTestAlert);
  server.on("/testlcd", HTTP_GET, handleTestLCD);
  server.on("/testservo", HTTP_GET, handleTestServo);
  server.on("/testrelay", HTTP_GET, handleTestRelay);
  server.on("/restart", HTTP_GET, handleRestart);
  server.on("/rtctime", HTTP_GET, handleGetRTCTime);
  server.on("/logs", HTTP_GET, handleGetLogs);
  server.on("/clearlogs", HTTP_GET, handleClearLogs);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not Found - Use http://" + WiFi.localIP().toString());
  });
  
  server.begin();
  Serial.println("Web server started!");
  addLog("Web server started");
  Serial.println("Access at: http://" + WiFi.localIP().toString());
  Serial.println("====================");
  
  // Start event monitoring
  xTaskCreatePinnedToCore(checkEventTask, "Events", 4096, NULL, 1, NULL, 1);
  addLog("Event monitor started");
}

void loop() {
  server.handleClient();
  
  // Periodic NTP sync
  unsigned long now = millis();
  if (now - lastNTPSync > ntpSyncInterval) {
    addLog("NTP sync");
    syncRTCWithNTP();
    lastNTPSync = now;
  }
  
  // Handle buzzer beeps during alert
  if (alertActive) {
    unsigned long elapsed = now - alertStartTime;
    if (elapsed % 500 < 100) {  // Beep for 100ms every 500ms
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
  
  // Update display more frequently during alerts (every 250ms)
  unsigned long displayIntervalAlert = alertActive ? 250 : 1000;
  
  if (now - lastDisplayUpdate >= displayIntervalAlert) {
    lastDisplayUpdate = now;
    updateDisplay();
  }
  
  // Handle test alert state machine
  if (testState != TEST_IDLE) {
    switch (testState) {
      case TEST_SERVO_START:
        myservo.write(0);
        addLog("Test: Servo to 0");
        testTimer = now + 3000;
        testState = TEST_SERVO_MID;
        break;

      case TEST_SERVO_MID:
        if (now >= testTimer) {
          myservo.write(180);
          addLog("Test: Servo to 180");
          testTimer = now + 3000;
          testState = TEST_SERVO_END;
        }
        break;

      case TEST_SERVO_END:
        if (now >= testTimer) {
          myservo.write(0);
          addLog("Test: Servo to 0");
          testState = TEST_RELAY_HIGH;
        }
        break;

      case TEST_RELAY_HIGH:
        digitalWrite(RELAY_PIN, HIGH);
        addLog("Test: Relay triggered for water dispense");
        testTimer = now + 10000;
        testState = TEST_RELAY_WAIT;
        break;

      case TEST_RELAY_WAIT:
        if (now >= testTimer) {
          digitalWrite(RELAY_PIN, LOW);
          addLog("Test: Relay off");
          testState = TEST_IDLE;
          addLog("Test alert completed");
        }
        break;
    }
  }
  
  delay(10);
}

void handleRoot() {
  addLog("Dashboard viewed");
  server.send(200, "text/html", dashboardHTML);
}

void handleAddEvent() {
  server.send(200, "text/html", addEventHTML);
}

void handleSubmit() {
  if (server.hasArg("repeat") && server.hasArg("time") && 
      server.hasArg("storageId") && server.hasArg("amount")) {
    
    // Validate storage ID (1-10)
    int storageId = server.arg("storageId").toInt();
    if (storageId < 1 || storageId > 10) {
      addLog("Invalid storage: " + String(storageId));
      sendSuccessPage("Error", "<p>Storage ID must be between 1-10</p><a href='/addEvent' class='btn' style='background:#f44336;'>Try Again</a>");
      return;
    }
    
    JsonObject event = eventDataArray.createNestedObject();
    event["id"] = String(nextID++);
    event["repeat"] = server.arg("repeat");
    event["time"] = server.arg("time");
    event["storageId"] = storageId;
    event["amount"] = server.arg("amount").toInt();
    event["enabled"] = true;
    
    String repeatType = event["repeat"].as<String>();
    if (repeatType.startsWith("*") || repeatType == "Weekly") {
      event["dayCount"] = 0;
    }
    
    String message = "<p><strong>ID:</strong> " + event["id"].as<String>() + "</p><p><strong>Storage:</strong> " + String(storageId) + " - <strong>Time:</strong> " + event["time"].as<String>() + "</p>";
    sendSuccessPage("Event Added!", message);
    addLog("Event added: #" + event["id"].as<String>() + " at " + event["time"].as<String>());
  } else {
    addLog("Add event failed");
    sendSuccessPage("Error", "<p>Missing form data</p><a href='/addEvent' class='btn' style='background:#f44336;'>Try Again</a>");
  }
}

void handleGetEvents() {
  String json;
  serializeJson(eventDataArray, json);
  server.send(200, "application/json", json);
}

void handleDeleteEvent() {
  if (server.hasArg("id")) {
    String idToDelete = server.arg("id");
    bool found = false;
    size_t indexToRemove = -1;

    for (size_t i = 0; i < eventDataArray.size(); i++) {
      if (eventDataArray[i]["id"].as<String>() == idToDelete) {
        indexToRemove = i;
        found = true;
        break;
      }
    }

    if (found) {
      eventDataArray.remove(indexToRemove);
      addLog("Event deleted: #" + idToDelete);
      // Reset nextID to the highest existing ID + 1
      nextID = 1;
      for (size_t i = 0; i < eventDataArray.size(); i++) {
        int currentID = eventDataArray[i]["id"].as<String>().toInt();
        if (currentID >= nextID) {
          nextID = currentID + 1;
        }
      }
      sendSuccessPage("Event Deleted", "<p><strong>Event ID:</strong> " + idToDelete + " removed</p>");
    } else {
      addLog("Event not found: #" + idToDelete);
      sendSuccessPage("Event Not Found", "<p><strong>Event ID:</strong> " + idToDelete + " not found</p>");
    }
  } else {
    addLog("Delete failed - no ID");
    sendSuccessPage("Error", "<p>Missing event ID</p>");
  }
}

void handleClearEvents() {
  eventDataArray.clear();
  nextID = 1;
  addLog("All events cleared");
  sendSuccessPage("Events Cleared", "<p><strong>All events</strong> removed</p>");
}

void handleDebug() {
  addLog("Debug page viewed");
  server.send(200, "text/html", debugHTML);
}

void handleDebugInfo() {
  DynamicJsonDocument debugDoc(512);
  DateTime now = rtc.now();
  debugDoc["rtcTime"] = String(now.year()) + "-" + 
                       (now.month() < 10 ? "0" : "") + String(now.month()) + "-" + 
                       (now.day() < 10 ? "0" : "") + String(now.day()) + " " +
                       (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" + 
                       (now.minute() < 10 ? "0" : "") + String(now.minute()) + ":" + 
                       (now.second() < 10 ? "0" : "") + String(now.second());
  debugDoc["wifiStatus"] = WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
  debugDoc["wifiSignal"] = WiFi.RSSI();
  debugDoc["freeHeap"] = ESP.getFreeHeap();
  debugDoc["eventCount"] = eventDataArray.size();
  unsigned long uptimeSeconds = millis() / 1000;
  unsigned long hours = uptimeSeconds / 3600;
  unsigned long minutes = (uptimeSeconds % 3600) / 60;
  debugDoc["uptime"] = String(hours) + "h " + String(minutes) + "m";
  
  String json;
  serializeJson(debugDoc, json);
  server.send(200, "application/json", json);
}

void handleTestLCD() {
  addLog("LCD test triggered");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("LCD Test OK!");
  
  for (int i = 0; i < 5; i++) {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(i, 1);
    lcd.print("O");  // Use 'O' instead of emoji for compatibility
    delay(500);
  }
  
  updateDisplay();
  addLog("LCD test completed");
  sendSuccessPage("LCD Test", "<p><strong>LCD:</strong> Displayed 'LCD Test OK!' with animation for 5s</p>");
}

void handleTestServo() {
  addLog("Servo test triggered");
  myservo.write(0);
  delay(3000);
  myservo.write(180);
  delay(3000);
  myservo.write(0);
  addLog("Servo test completed");
  sendSuccessPage("Servo Test", "<p><strong>Servo:</strong> Moved 0 to 180 to 0 degrees</p>");
}

void handleTestRelay() {
  addLog("Relay test triggered");
  digitalWrite(RELAY_PIN, HIGH);  // Activate (active-high)
  delay(5000);
  digitalWrite(RELAY_PIN, LOW);  // Deactivate
  addLog("Relay test completed");
  sendSuccessPage("Relay Test", "<p><strong>Relay:</strong> Activated for 5 seconds</p>");
}

void handleRestart() {
  addLog("ESP32 restart triggered");
  sendSuccessPage("Restarting", "<p><strong>ESP32:</strong> Restarting now...</p>");
  delay(1000);
  ESP.restart();
}

void handleTestAlert() {
  addLog("Test alert triggered");
  startAlert();
  testState = TEST_SERVO_START;
  testTimer = millis();
  sendSuccessPage("Alert Test", "<p><strong>LCD:</strong> 'Take the pills!' flashing for 20s<br><strong>Servo:</strong> Cycled once<br><strong>Relay:</strong> Activated for 10s</p>");
}

void handleGetRTCTime() {
  DateTime now = rtc.now();
  String timeStr = String(now.year()) + "-" + 
                   (now.month() < 10 ? "0" : "") + String(now.month()) + "-" + 
                   (now.day() < 10 ? "0" : "") + String(now.day()) + " " +
                   (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" + 
                   (now.minute() < 10 ? "0" : "") + String(now.minute()) + ":" + 
                   (now.second() < 10 ? "0" : "") + String(now.second());
  server.send(200, "text/plain", timeStr);
}

void handleGetLogs() {
  String logText = "";
  for (int i = 0; i < logCount; i++) {
    logText += logs[i] + "\n";
  }
  server.send(200, "text/plain", logText);
}

void handleClearLogs() {
  logCount = 0;
  for (int i = 0; i < MAX_LOGS; i++) {
    logs[i] = "";
  }
  addLog("Logs cleared");
  sendSuccessPage("Logs Cleared", "<p><strong>Activity log</strong> cleared</p>");
}

void checkEventTask(void *parameter) {
  static int lastMinute = -1;
  for (;;) {
    DateTime now = rtc.now();
    int currentMinute = now.minute();
    
    if (currentMinute != lastMinute) {
      lastMinute = currentMinute;
      
      char currentTimeStr[6];
      sprintf(currentTimeStr, "%02d:%02d", now.hour(), now.minute());
      String currentTime = String(currentTimeStr);
      
      for (size_t i = 0; i < eventDataArray.size(); i++) {
        JsonObject event = eventDataArray[i];
        
        if (!event.containsKey("enabled") || !event["enabled"].as<bool>()) {
          continue;
        }
        
        String eventTime = event["time"].as<String>();
        
        if (eventTime == currentTime) {
          addLog("Event #" + event["id"].as<String>() + " triggered");
          triggerEvent(event);
        }
      }
    }
    
    if (status == 1) {
      addLog("Medicine notification");
      notificationOn();
      status = 2;
      vTaskDelay(10000 / portTICK_PERIOD_MS);
      status = 0;
      notificationOff();
    }
    
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void triggerEvent(JsonObject event) {
  int storageId = event["storageId"].as<int>();
  int amount = event["amount"].as<int>();
  
  if (storageId < 1 || storageId > 10) {
    addLog("Invalid storage: " + String(storageId));
    return;
  }
  
  String repeatType = event["repeat"].as<String>();
  int interval = 0;
  if (repeatType.startsWith("*")) {
    interval = repeatType.substring(1).toInt();
  } else if (repeatType == "Weekly") {
    interval = 7;
  }
  
  if (interval > 0) {
    int dayCount = event.containsKey("dayCount") ? event["dayCount"].as<int>() : 0;
    if (dayCount > 0) {
      event["dayCount"] = dayCount - 1;
      addLog("Event #" + event["id"].as<String>() + " skipped");
      return;
    }
    event["dayCount"] = interval - 1;
  }
  
  outputMedicine(storageId, amount);
  
  startAlert();
  
  digitalWrite(RELAY_PIN, HIGH);  // Activate (active-high)
  addLog("Relay triggered for water dispense");
  delay(10000);
  digitalWrite(RELAY_PIN, LOW);  // Deactivate
  addLog("Relay off");
  
  status = 1;
}

void outputMedicine(int storageId, int amount) {
  addLog("Dispensing " + String(amount) + " from storage #" + String(storageId));
  
  for (int i = 0; i < amount; i++) {
    myservo.write(0);
    delay(3000);
    myservo.write(180);
    delay(3000);
    myservo.write(0);
    addLog("Servo cycle " + String(i + 1) + " completed for storage #" + String(storageId));
    delay(1000);  // Delay between cycles
  }
}

void startAlert() {
  alertActive = true;
  alertStartTime = millis();
  addLog("LCD alert started");
  updateDisplay();
}

void stopAlert() {
  alertActive = false;
  lcd.backlight();
  digitalWrite(BUZZER_PIN, LOW);
  addLog("LCD alert ended");
  updateDisplay();
}

void notificationOn() {
  for (int i = 0; i < 10; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
}

void notificationOff() {
  digitalWrite(LED_PIN, LOW);
}

void updateDisplay() {
  unsigned long now = millis();
  
  if (alertActive && (now - alertStartTime > alertDuration)) {
    stopAlert();
    return;
  }
  
  if (alertActive) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("TAKE THE PILLS!");
    lcd.setCursor(0, 1);
    lcd.print("Storage: ALL");
    
    if ((now / 250) % 2 == 0) {
      lcd.backlight();
    } else {
      lcd.noBacklight();
    }
  } else {
    DateTime nowTime = rtc.now();
    lcd.clear();
    lcd.setCursor(0, 0);
    String timeStr = nowTime.timestamp(DateTime::TIMESTAMP_TIME);
    lcd.print(timeStr.substring(0,5));  // HH:MM
    lcd.setCursor(11, 0);
    lcd.print(String(eventDataArray.size()) + "/10");
    lcd.setCursor(0, 1);
    String ip = WiFi.localIP().toString();
    int lastDot = ip.lastIndexOf('.');
    int prevDot = ip.lastIndexOf('.', lastDot - 1);
    String conId = ip.substring(prevDot + 1);
    lcd.print("CON ID: " + conId);
    lcd.backlight();
  }
}

void syncRTCWithNTP() {
  addLog("NTP sync started");
  Serial.println("Syncing RTC with NTP...");

  long timezoneOffset = 28800;  // UTC+8 (Hong Kong Time)

  configTime(timezoneOffset, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  struct tm timeinfo;
  int retries = 3;
  bool success = false;

  for (int i = 0; i < retries; i++) {
    if (getLocalTime(&timeinfo, 15000)) {
      rtc.adjust(DateTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      ));
      success = true;
      break;
    }
    addLog("NTP retry " + String(i + 1) + " failed");
    delay(2000);
  }

  if (success) {
    addLog("NTP sync OK");
    Serial.println("RTC synced with NTP successfully");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("NTP Sync OK");
    delay(2000);
    updateDisplay();
  } else {
    addLog("NTP sync failed");
    Serial.println("NTP sync failed after retries, using current RTC time");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("NTP Sync Failed");
    lcd.setCursor(0, 1);
    lcd.print("Check Network");
    delay(5000);
    updateDisplay();
  }
}

void sendSuccessPage(String title, String message) {
  String html = String(successHTMLTemplate);
  html.replace("%TITLE%", title);
  html.replace("%MESSAGE%", message);
  server.send(200, "text/html", html);
}   