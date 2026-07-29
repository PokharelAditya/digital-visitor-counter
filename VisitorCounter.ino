/*
  =====================================================================
   DIGITAL VISITOR COUNTER — ESP32 Firmware  (v2 — with logging + cloud)
  =====================================================================
  Board      : ESP32 DevKit v1 (38-pin) or any ESP32 dev board
  Framework  : Arduino (works in Arduino IDE and PlatformIO)

  FEATURES
   - Dual IR sensor entry/exit direction detection (FSM + debounce)
   - Live OLED display (SSD1306, 128x64, I2C)
   - LED + buzzer alerts, including "room full" alarm
   - Persistent COUNTERS storage using ESP32 NVS (Preferences)
   - Persistent EVENT LOG (entry/exit timestamps) stored locally on
     the ESP32's flash filesystem (LittleFS) as a CSV file
   - Real time timestamps via NTP (internet time sync)
   - Optional Firebase Realtime Database cloud sync — every entry/exit
     event AND the live occupancy status are pushed to the cloud so
     they can be viewed from anywhere, not just on the local Wi-Fi
   - Wi-Fi connection with:
       * Built-in live web dashboard  (http://<device-ip>/)
       * JSON REST API                (http://<device-ip>/status)
       * Event log JSON API           (http://<device-ip>/log)
       * CSV log download             (http://<device-ip>/export)
       * Remote reset endpoint        (http://<device-ip>/reset)
       * Clear log endpoint           (http://<device-ip>/clearlog)
   - Optional ThingSpeak cloud upload (toggle ENABLE_THINGSPEAK below)
   - Serial admin console: RESET, SETCAP <n>, STATUS, CLEARLOG, HELP

  REQUIRED LIBRARIES (install via Arduino IDE Library Manager):
   - Adafruit GFX Library
   - Adafruit SSD1306
   - (WiFi.h, WebServer.h, Preferences.h, HTTPClient.h, WiFiClientSecure.h,
      LittleFS.h, time.h are all bundled with the ESP32 board package —
      no separate install needed)

  BOARD PACKAGE:
   - Install "esp32 by Espressif Systems" via Boards Manager
   - Select your board, e.g. "ESP32 Dev Module"
   - IMPORTANT: In Tools > Partition Scheme, pick a scheme that includes
     SPIFFS/LittleFS space (e.g. "Default 4MB with spiffs") or the local
     log file will fail to mount.

  WIRING  (see project report Section 10.2 for full details)
   - IR1 (entry sensor) OUT -> GPIO 4      IR1 VCC -> 3V3, GND -> GND
   - IR2 (exit sensor)  OUT -> GPIO 5      IR2 VCC -> 3V3, GND -> GND
   - OLED SDA -> GPIO 21     OLED SCL -> GPIO 22     OLED VCC -> 3V3
   - LED (+220ohm resistor)  -> GPIO 2     -> GND
   - Buzzer (+)              -> GPIO 15    -> GND
   - Admin reset button (optional) -> GPIO 13 (other leg to GND)

  FIREBASE SETUP
   - See the accompanying "Firebase_Setup_Guide.pdf" for full, click-by-
     click instructions on creating a free Firebase project, enabling
     the Realtime Database, and getting the two values you need below
     (FIREBASE_HOST and FIREBASE_AUTH).
  =====================================================================
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <time.h>
#include "config.h"
/* --------------------------- CONFIGURATION --------------------------- */

// ---- Wi-Fi credentials (leave blank to run fully offline) ----

// ---- Feature toggles ----
#define ENABLE_WIFI_DASHBOARD 1     // set to 0 to disable Wi-Fi entirely
#define ENABLE_THINGSPEAK     0     // set to 1 to also push data to ThingSpeak
#define ENABLE_LOCAL_LOG      1     // set to 1 to store entry/exit log on flash (LittleFS)
#define ENABLE_FIREBASE       1   // set to 1 to sync every event + status to Firebase

// ---- ThingSpeak settings (only used if ENABLE_THINGSPEAK = 1) ----
const unsigned long THINGSPEAK_INTERVAL_MS = 20000; // ThingSpeak min. interval

// ---- Firebase Realtime Database settings (only used if ENABLE_FIREBASE = 1) ----
// Get these from the Firebase console. See Firebase_Setup_Guide.pdf.
//   FIREBASE_HOST : your database URL WITHOUT "https://" and WITHOUT a
//                   trailing slash, e.g. "visitor-counter-abcd-default-rtdb.firebaseio.com"
//   FIREBASE_AUTH : a database secret OR an auth/ID token used to
//                   authenticate the REST request (see guide for both options)

// ---- NTP / timezone settings (for real entry & exit timestamps) ----
// Default below is Nepal Standard Time (UTC+5:45). Change for your location.
const char* NTP_SERVER        = "pool.ntp.org";
const long  GMT_OFFSET_SEC    = 5 * 3600 + 45 * 60;  // UTC+5:45 (Nepal)
const int   DAYLIGHT_OFFSET_SEC = 0;                 // most regions: 0

// ---- Pin assignments ----
#define IR1_PIN      4     // entry-side IR sensor
#define IR2_PIN      5     // exit-side IR sensor
#define LED_PIN      2
#define BUZZER_PIN   15
#define RESET_BTN_PIN 13   // optional manual reset button
#define OLED_SDA     21
#define OLED_SCL     22

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C

// ---- Algorithm tuning constants ----
const unsigned long DEBOUNCE_MS      = 300;   // ignore re-triggers of same sensor within this window
const unsigned long EVENT_TIMEOUT_MS = 1500;  // max gap allowed between IR1 and IR2 trigger
const unsigned long BEEP_SHORT_MS    = 80;
const unsigned long SAVE_EVERY_N_EVENTS = 1;  // how often to persist to flash

// ---- Log settings ----
const char* LOG_FILE_PATH   = "/log.csv";
const size_t LOG_MAX_BYTES  = 200000;  // ~200KB safety cap; oldest half is trimmed once exceeded
const int    LOG_JSON_MAX_ROWS = 50;   // how many recent rows /log returns to the dashboard

int MAX_CAPACITY = 30;   // can be changed at runtime via Serial/web

/* --------------------------- GLOBAL OBJECTS --------------------------- */

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Preferences prefs;
#if ENABLE_WIFI_DASHBOARD
WebServer server(80);
#endif

/* --------------------------- STATE VARIABLES --------------------------- */

volatile bool ir1Flag = false;   // set by ISR
volatile bool ir2Flag = false;

unsigned long lastIR1Accept = 0;
unsigned long lastIR2Accept = 0;

enum FsmState { IDLE, IR1_TRIGGERED, IR2_TRIGGERED };
FsmState fsmState = IDLE;
unsigned long firstTriggerTime = 0;

uint32_t entries = 0;
uint32_t exits   = 0;
int32_t  occupancy = 0;
uint32_t eventsSinceSave = 0;

bool roomFull = false;
bool timeSynced = false;
unsigned long lastOledUpdate = 0;
unsigned long lastThingSpeakPush = 0;

/* --------------------------- INTERRUPT SERVICE ROUTINES --------------------------- */
// Keep ISRs minimal: just set a flag, all real work happens in loop().

void IRAM_ATTR isrIR1() { ir1Flag = true; }
void IRAM_ATTR isrIR2() { ir2Flag = true; }

/* --------------------------- FORWARD DECLARATIONS --------------------------- */

void handleSerialCommands();
void updateFSM();
void onEvent(const char* type);
void beepShort();
void beepAlarmPattern();
void refreshOLED();
void saveCounters();
void loadCounters();
void connectWiFi();
void syncTime();
bool getTimestamp(char* outBuf, size_t bufLen, time_t* epochOut);
#if ENABLE_LOCAL_LOG
void initLocalLog();
void appendLocalLog(const char* type, const char* timestamp, time_t epoch);
String readRecentLogAsJson(int maxRows);
void clearLocalLog();
#endif
#if ENABLE_FIREBASE
void firebasePushEvent(const char* type, const char* timestamp, time_t epoch);
void firebasePushStatus();
#endif
#if ENABLE_WIFI_DASHBOARD
void setupWebServer();
void handleRoot();
void handleStatus();
void handleReset();
void handleLogJson();
void handleExportCsv();
void handleClearLog();
#endif
#if ENABLE_THINGSPEAK
void pushToThingSpeak();
#endif

/* ============================== SETUP ============================== */

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== Digital Visitor Counter booting ==="));

  pinMode(IR1_PIN, INPUT_PULLUP);
  pinMode(IR2_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Most IR obstacle modules pull OUT LOW when an object is detected.
  // Interrupt on CHANGE lets us catch both the trigger and release edges;
  // loop() decides which edge actually matters.
  attachInterrupt(digitalPinToInterrupt(IR1_PIN), isrIR1, CHANGE);
  attachInterrupt(digitalPinToInterrupt(IR2_PIN), isrIR2, CHANGE);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED init failed! Check wiring/I2C address."));
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("Digital Visitor Counter"));
    display.println(F("Booting..."));
    display.display();
  }

  loadCounters();

#if ENABLE_LOCAL_LOG
  initLocalLog();
#endif

#if ENABLE_WIFI_DASHBOARD
  connectWiFi();
  syncTime();
  setupWebServer();
#endif

  Serial.println(F("Type HELP for admin commands."));
  refreshOLED();
}

/* ============================== MAIN LOOP ============================== */

void loop() {
  updateFSM();
  handleSerialCommands();

  // Optional manual reset button (active LOW)
  static unsigned long lastBtnCheck = 0;
  if (millis() - lastBtnCheck > 50) {
    lastBtnCheck = millis();
    if (digitalRead(RESET_BTN_PIN) == LOW) {
      delay(30); // simple debounce
      if (digitalRead(RESET_BTN_PIN) == LOW) {
        entries = 0; exits = 0; occupancy = 0; roomFull = false;
        saveCounters();
        refreshOLED();
        Serial.println(F("[BUTTON] Counters reset."));
        while (digitalRead(RESET_BTN_PIN) == LOW) delay(10); // wait for release
      }
    }
  }

  // Refresh OLED a few times per second (not every loop, to reduce I2C traffic)
  if (millis() - lastOledUpdate > 500) {
    lastOledUpdate = millis();
    refreshOLED();
  }

#if ENABLE_WIFI_DASHBOARD
  server.handleClient();
#endif

#if ENABLE_THINGSPEAK
  if (WiFi.status() == WL_CONNECTED &&
      millis() - lastThingSpeakPush > THINGSPEAK_INTERVAL_MS) {
    lastThingSpeakPush = millis();
    pushToThingSpeak();
  }
#endif
}

/* ============================== SENSOR + FSM LOGIC ============================== */
// Direction detection: IR1 -> IR2 within timeout = ENTRY
//                       IR2 -> IR1 within timeout = EXIT

bool readIR1Triggered() {
  if (!ir1Flag) return false;
  ir1Flag = false;
  bool triggered = (digitalRead(IR1_PIN) == LOW); // LOW = beam broken (adjust if your module is active-HIGH)
  if (triggered && millis() - lastIR1Accept > DEBOUNCE_MS) {
    lastIR1Accept = millis();
    return true;
  }
  return false;
}

bool readIR2Triggered() {
  if (!ir2Flag) return false;
  ir2Flag = false;
  bool triggered = (digitalRead(IR2_PIN) == LOW);
  if (triggered && millis() - lastIR2Accept > DEBOUNCE_MS) {
    lastIR2Accept = millis();
    return true;
  }
  return false;
}

void updateFSM() {
  bool ir1 = readIR1Triggered();
  bool ir2 = readIR2Triggered();

  switch (fsmState) {
    case IDLE:
      if (ir1) {
        fsmState = IR1_TRIGGERED;
        firstTriggerTime = millis();
      } else if (ir2) {
        fsmState = IR2_TRIGGERED;
        firstTriggerTime = millis();
      }
      break;

    case IR1_TRIGGERED:            // candidate ENTRY (waiting for IR2)
      if (ir2) {
        entries++;
        occupancy++;
        onEvent("ENTRY");
        fsmState = IDLE;
      } else if (millis() - firstTriggerTime > EVENT_TIMEOUT_MS) {
        fsmState = IDLE;           // discard: false start / person turned back
      }
      break;

    case IR2_TRIGGERED:            // candidate EXIT (waiting for IR1)
      if (ir1) {
        exits++;
        if (occupancy > 0) occupancy--; // prevent negative occupancy
        onEvent("EXIT");
        fsmState = IDLE;
      } else if (millis() - firstTriggerTime > EVENT_TIMEOUT_MS) {
        fsmState = IDLE;
      }
      break;
  }

  // Capacity check (runs every loop so it reacts immediately)
  bool shouldBeFull = (occupancy >= MAX_CAPACITY);
  if (shouldBeFull != roomFull) {
    roomFull = shouldBeFull;
    if (roomFull) {
      Serial.println(F("[ALERT] Maximum capacity reached!"));
    }
  }
  digitalWrite(LED_PIN, roomFull ? HIGH : LOW);
}

void onEvent(const char* type) {
  beepShort();
  refreshOLED();

  // Get a real-world timestamp for this event (falls back to uptime if
  // NTP hasn't synced yet, e.g. no internet available).
  char ts[32];
  time_t epoch = 0;
  bool haveRealTime = getTimestamp(ts, sizeof(ts), &epoch);
  if (!haveRealTime) {
    snprintf(ts, sizeof(ts), "uptime+%lus", millis() / 1000);
  }

  Serial.print(F("[EVENT] "));
  Serial.print(type);
  Serial.print(F("  Time="));   Serial.print(ts);
  Serial.print(F("  Entries=")); Serial.print(entries);
  Serial.print(F("  Exits="));   Serial.print(exits);
  Serial.print(F("  Occupancy=")); Serial.println(occupancy);

  eventsSinceSave++;
  if (eventsSinceSave >= SAVE_EVERY_N_EVENTS) {
    saveCounters();
    eventsSinceSave = 0;
  }

#if ENABLE_LOCAL_LOG
  appendLocalLog(type, ts, epoch);
#endif

#if ENABLE_FIREBASE
  if (WiFi.status() == WL_CONNECTED) {
    firebasePushEvent(type, ts, epoch);
    firebasePushStatus();
  }
#endif

  if (roomFull) beepAlarmPattern();
}

/* ============================== ACTUATORS ============================== */

void beepShort() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(BEEP_SHORT_MS);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepAlarmPattern() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}

void refreshOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("DIGITAL VISITOR COUNTER"));
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  display.setCursor(0, 16);
  display.print(F("IN : ")); display.print(entries);
  display.setCursor(70, 16);
  display.print(F("OUT: ")); display.print(exits);

  display.setTextSize(2);
  display.setCursor(0, 32);
  display.print(F("OCC: "));
  display.print(occupancy);

  display.setTextSize(1);
  display.setCursor(0, 56);
  if (roomFull) {
    display.print(F("STATUS: ROOM FULL!"));
  } else {
#if ENABLE_WIFI_DASHBOARD
    display.print(F("WiFi: "));
    display.print(WiFi.status() == WL_CONNECTED ? "OK  " : "--  ");
    display.print(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "");
#else
    display.print(F("STATUS: OK"));
#endif
  }
  display.display();
}

/* ============================== PERSISTENT STORAGE (COUNTERS) ============================== */
// NVS (Preferences) is used only for the small, frequently-written
// counters. It is NOT suitable for the full event log (many small
// key/value writes wear out flash faster and NVS isn't built for
// growing lists) — that's what LittleFS + Firebase are for below.

void loadCounters() {
  prefs.begin("visitor", false);
  entries = prefs.getUInt("entries", 0);
  exits   = prefs.getUInt("exits", 0);
  MAX_CAPACITY = prefs.getInt("maxcap", MAX_CAPACITY);
  occupancy = (int32_t)entries - (int32_t)exits;
  if (occupancy < 0) occupancy = 0;
  Serial.print(F("Loaded from flash -> Entries=")); Serial.print(entries);
  Serial.print(F(" Exits=")); Serial.print(exits);
  Serial.print(F(" Occupancy=")); Serial.println(occupancy);
}

void saveCounters() {
  prefs.putUInt("entries", entries);
  prefs.putUInt("exits", exits);
  prefs.putInt("maxcap", MAX_CAPACITY);
}

/* ============================== REAL-TIME CLOCK (NTP) ============================== */

void syncTime() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("No WiFi — skipping NTP sync, timestamps will use uptime."));
    return;
  }
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.print(F("Syncing time with NTP"));
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 20) {
    delay(300);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  if (attempts < 20) {
    timeSynced = true;
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.print(F("Time synced: ")); Serial.println(buf);
  } else {
    Serial.println(F("NTP sync failed — timestamps will use uptime until it succeeds."));
  }
}

// Fills outBuf with "YYYY-MM-DD HH:MM:SS" and epochOut with the Unix time.
// Returns false (and leaves outBuf untouched) if real time isn't available yet.
bool getTimestamp(char* outBuf, size_t bufLen, time_t* epochOut) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) return false;
  strftime(outBuf, bufLen, "%Y-%m-%d %H:%M:%S", &timeinfo);
  if (epochOut) *epochOut = time(nullptr);
  return true;
}

/* ============================== LOCAL EVENT LOG (LittleFS) ============================== */
// Stores every entry/exit as a CSV row so the full history survives a
// reboot/power-cut, even with no Wi-Fi/Firebase available. Format:
//   timestamp,type,entries,exits,occupancy
#if ENABLE_LOCAL_LOG

void initLocalLog() {
  if (!LittleFS.begin(true)) {  // true = format on failure (first boot)
    Serial.println(F("LittleFS mount failed! Local logging disabled."));
    return;
  }
  if (!LittleFS.exists(LOG_FILE_PATH)) {
    File f = LittleFS.open(LOG_FILE_PATH, "w");
    if (f) {
      f.println("timestamp,type,entries,exits,occupancy");
      f.close();
    }
  }
  File f = LittleFS.open(LOG_FILE_PATH, "r");
  if (f) {
    Serial.print(F("Local log file size: "));
    Serial.print(f.size());
    Serial.println(F(" bytes"));
    f.close();
  }
}

void appendLocalLog(const char* type, const char* timestamp, time_t epoch) {
  // Trim the file if it's grown too large: keep only the newer half.
  File check = LittleFS.open(LOG_FILE_PATH, "r");
  if (check && check.size() > LOG_MAX_BYTES) {
    check.close();
    File in = LittleFS.open(LOG_FILE_PATH, "r");
    String all = in.readString();
    in.close();
    int half = all.length() / 2;
    int cut = all.indexOf('\n', half);
    String kept = (cut > 0) ? all.substring(cut + 1) : all;
    File out = LittleFS.open(LOG_FILE_PATH, "w");
    out.println("timestamp,type,entries,exits,occupancy");
    out.print(kept);
    out.close();
    Serial.println(F("[LOG] File exceeded size cap — trimmed oldest half."));
  } else if (check) {
    check.close();
  }

  File f = LittleFS.open(LOG_FILE_PATH, "a");
  if (!f) {
    Serial.println(F("[LOG] Failed to open log file for append."));
    return;
  }
  f.print(timestamp);   f.print(",");
  f.print(type);         f.print(",");
  f.print(entries);      f.print(",");
  f.print(exits);        f.print(",");
  f.println(occupancy);
  f.close();
}

void clearLocalLog() {
  File f = LittleFS.open(LOG_FILE_PATH, "w");
  if (f) {
    f.println("timestamp,type,entries,exits,occupancy");
    f.close();
  }
  Serial.println(F("Local log cleared."));
}

// Reads the last `maxRows` rows of the CSV and returns them as a JSON
// array of objects, newest last, for the dashboard's history table.
String readRecentLogAsJson(int maxRows) {
  File f = LittleFS.open(LOG_FILE_PATH, "r");
  if (!f) return "[]";

  // Read all lines into a small ring buffer of Strings (fine for a few
  // hundred rows on ESP32's RAM; the CSV itself is size-capped above).
  const int CAP = 500;
  static String lines[CAP];
  int count = 0;
  f.readStringUntil('\n'); // skip header
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    lines[count % CAP] = line;
    count++;
  }
  f.close();

  int total = min(count, CAP);
  int start = (count > maxRows) ? count - maxRows : 0;
  start = max(start, count - total); // stay within what's actually stored

  String json = "[";
  bool first = true;
  for (int i = start; i < count; i++) {
    String line = lines[i % CAP];
    // Split: timestamp,type,entries,exits,occupancy
    int p1 = line.indexOf(',');
    int p2 = line.indexOf(',', p1 + 1);
    int p3 = line.indexOf(',', p2 + 1);
    int p4 = line.indexOf(',', p3 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0) continue;
    String ts   = line.substring(0, p1);
    String type = line.substring(p1 + 1, p2);
    String ent  = line.substring(p2 + 1, p3);
    String ex   = line.substring(p3 + 1, p4);
    String occ  = line.substring(p4 + 1);

    if (!first) json += ",";
    first = false;
    json += "{\"timestamp\":\"" + ts + "\",\"type\":\"" + type +
            "\",\"entries\":" + ent + ",\"exits\":" + ex +
            ",\"occupancy\":" + occ + "}";
  }
  json += "]";
  return json;
}

#endif // ENABLE_LOCAL_LOG

/* ============================== FIREBASE (CLOUD DATABASE) ============================== */
// Firebase Realtime Database is a CLOUD-hosted JSON database (not local
// storage) — the ESP32 pushes data to Google's servers over the internet,
// and it stays available even if the ESP32 is off or the phone/laptop is
// somewhere else entirely. See Firebase_Setup_Guide.pdf for full setup.
#if ENABLE_FIREBASE

bool firebaseRequest(const char* method, const String& path, const String& jsonBody) {
  WiFiClientSecure client;
  client.setInsecure();  // skips certificate validation — fine for hobby/prototype use;
                          // see the setup guide for the more secure alternative.
  HTTPClient http;
  String url = "https://" + String(FIREBASE_HOST) + path + ".json?auth=" + String(FIREBASE_AUTH);

  if (!http.begin(client, url)) {
    Serial.println(F("[Firebase] http.begin() failed."));
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  int code;
  if (strcmp(method, "POST") == 0) code = http.POST(jsonBody);
  else if (strcmp(method, "PUT") == 0) code = http.PUT(jsonBody);
  else code = http.sendRequest(method, jsonBody);

  if (code <= 0) {
    Serial.print(F("[Firebase] Request failed: "));
    Serial.println(http.errorToString(code));
  } else if (code >= 400) {
    Serial.print(F("[Firebase] HTTP error ")); Serial.println(code);
  }
  http.end();
  return (code > 0 && code < 400);
}

// Adds one event as a new child under /logs — Firebase auto-generates a
// unique, time-ordered key for each push (like an auto-increment ID).
void firebasePushEvent(const char* type, const char* timestamp, time_t epoch) {
  String body = "{";
  body += "\"type\":\"" + String(type) + "\",";
  body += "\"timestamp\":\"" + String(timestamp) + "\",";
  body += "\"epoch\":" + String((long)epoch) + ",";
  body += "\"entries\":" + String(entries) + ",";
  body += "\"exits\":" + String(exits) + ",";
  body += "\"occupancy\":" + String(occupancy);
  body += "}";
  firebaseRequest("POST", "/logs", body);
}

// Overwrites /status with the current snapshot, so any app/dashboard
// reading directly from Firebase always sees the latest numbers.
void firebasePushStatus() {
  String body = "{";
  body += "\"entries\":" + String(entries) + ",";
  body += "\"exits\":" + String(exits) + ",";
  body += "\"occupancy\":" + String(occupancy) + ",";
  body += "\"maxCapacity\":" + String(MAX_CAPACITY) + ",";
  body += "\"full\":" + String(roomFull ? "true" : "false") + ",";
  body += "\"lastUpdate\":" + String((long)time(nullptr));
  body += "}";
  firebaseRequest("PUT", "/status", body);
}

#endif // ENABLE_FIREBASE

/* ============================== SERIAL ADMIN CONSOLE ============================== */

void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "HELP") {
    Serial.println(F("Commands: RESET | SETCAP <n> | STATUS | CLEARLOG | HELP"));
  } else if (cmd == "RESET") {
    entries = 0; exits = 0; occupancy = 0; roomFull = false;
    saveCounters();
    refreshOLED();
    Serial.println(F("Counters reset."));
  } else if (cmd.startsWith("SETCAP")) {
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx > 0) {
      MAX_CAPACITY = cmd.substring(spaceIdx + 1).toInt();
      saveCounters();
      Serial.print(F("MAX_CAPACITY set to ")); Serial.println(MAX_CAPACITY);
    }
  } else if (cmd == "STATUS") {
    Serial.print(F("Entries=")); Serial.print(entries);
    Serial.print(F(" Exits=")); Serial.print(exits);
    Serial.print(F(" Occupancy=")); Serial.print(occupancy);
    Serial.print(F(" MaxCap=")); Serial.println(MAX_CAPACITY);
#if ENABLE_LOCAL_LOG
  } else if (cmd == "CLEARLOG") {
    clearLocalLog();
#endif
  } else if (cmd.length() > 0) {
    Serial.println(F("Unknown command. Type HELP."));
  }
}

/* ============================== WI-FI + WEB DASHBOARD ============================== */
#if ENABLE_WIFI_DASHBOARD

void connectWiFi() {
  if (strlen(WIFI_SSID) == 0) {
    Serial.println(F("No WiFi SSID configured — running offline."));
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("Connecting to WiFi"));
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi connected! Dashboard: http://"));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi connection failed — continuing offline."));
  }
}

// The dashboard HTML/CSS/JS is served directly from flash — no external
// hosting needed. It polls /status every second via fetch() and updates
// the page without a full reload, and polls /log for the history table.
const char DASHBOARD_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Visitor Counter Dashboard</title>
<style>
  body{font-family:Arial,Helvetica,sans-serif;background:#0f172a;color:#e2e8f0;
       display:flex;flex-direction:column;align-items:center;padding:30px;margin:0;}
  h1{color:#38bdf8;margin-bottom:4px;}
  .sub{color:#94a3b8;margin-bottom:24px;font-size:14px;}
  .cards{display:flex;gap:16px;flex-wrap:wrap;justify-content:center;}
  .card{background:#1e293b;border-radius:14px;padding:22px 30px;min-width:140px;
        text-align:center;box-shadow:0 4px 14px rgba(0,0,0,0.35);}
  .card .label{font-size:13px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;}
  .card .value{font-size:38px;font-weight:bold;margin-top:6px;}
  .in .value{color:#4ade80;} .out .value{color:#f87171;} .occ .value{color:#38bdf8;}
  .status{margin-top:22px;padding:10px 22px;border-radius:20px;font-weight:bold;}
  .status.ok{background:#14532d;color:#4ade80;}
  .status.full{background:#7f1d1d;color:#fca5a5;}
  .bar-wrap{width:280px;background:#1e293b;border-radius:10px;height:18px;margin-top:20px;overflow:hidden;}
  .bar{height:100%;background:linear-gradient(90deg,#38bdf8,#f87171);width:0%;transition:width .4s;}
  .btnrow{display:flex;gap:10px;margin-top:26px;flex-wrap:wrap;justify-content:center;}
  button{background:#334155;color:#e2e8f0;border:none;padding:10px 22px;
         border-radius:8px;cursor:pointer;font-size:14px;}
  button:hover{background:#475569;}
  table{margin-top:30px;border-collapse:collapse;width:min(600px,92vw);font-size:13px;}
  th,td{padding:8px 10px;text-align:left;border-bottom:1px solid #1e293b;}
  th{color:#94a3b8;text-transform:uppercase;font-size:11px;letter-spacing:.5px;}
  td.type-ENTRY{color:#4ade80;font-weight:bold;}
  td.type-EXIT{color:#f87171;font-weight:bold;}
  .logtitle{margin-top:10px;color:#94a3b8;font-size:13px;}
</style>
</head>
<body>
  <h1>Digital Visitor Counter</h1>
  <div class="sub">Live occupancy dashboard — ESP32</div>
  <div class="cards">
    <div class="card in"><div class="label">Entries</div><div class="value" id="entries">--</div></div>
    <div class="card out"><div class="label">Exits</div><div class="value" id="exits">--</div></div>
    <div class="card occ"><div class="label">Occupancy</div><div class="value" id="occupancy">--</div></div>
  </div>
  <div class="bar-wrap"><div class="bar" id="bar"></div></div>
  <div class="status ok" id="status">OK</div>
  <div class="btnrow">
    <button onclick="resetCounters()">Reset Counters</button>
    <button onclick="window.location='/export'">Download Log (CSV)</button>
  </div>

  <div class="logtitle">Recent entry / exit log</div>
  <table>
    <thead><tr><th>Time</th><th>Type</th><th>Occupancy</th></tr></thead>
    <tbody id="logbody"><tr><td colspan="3">Loading...</td></tr></tbody>
  </table>

<script>
async function refresh(){
  try{
    const r = await fetch('/status');
    const d = await r.json();
    document.getElementById('entries').textContent = d.entries;
    document.getElementById('exits').textContent = d.exits;
    document.getElementById('occupancy').textContent = d.occupancy;
    const pct = Math.min(100, (d.occupancy / d.maxCapacity) * 100);
    document.getElementById('bar').style.width = pct + '%';
    const st = document.getElementById('status');
    if(d.full){ st.textContent = 'ROOM FULL'; st.className='status full'; }
    else { st.textContent = 'OK  (' + d.occupancy + ' / ' + d.maxCapacity + ')'; st.className='status ok'; }
  }catch(e){ console.log('fetch failed', e); }
}
async function refreshLog(){
  try{
    const r = await fetch('/log');
    const rows = await r.json();
    const body = document.getElementById('logbody');
    if(!rows.length){ body.innerHTML = '<tr><td colspan="3">No events yet.</td></tr>'; return; }
    body.innerHTML = rows.slice().reverse().map(e =>
      `<tr><td>${e.timestamp}</td><td class="type-${e.type}">${e.type}</td><td>${e.occupancy}</td></tr>`
    ).join('');
  }catch(e){ console.log('log fetch failed', e); }
}
function resetCounters(){
  if(confirm('Reset entry/exit counters?')){
    fetch('/reset').then(()=>{refresh(); refreshLog();});
  }
}
setInterval(refresh, 1000);
setInterval(refreshLog, 3000);
refresh();
refreshLog();
</script>
</body>
</html>
)HTMLPAGE";

void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleStatus() {
  String json = "{";
  json += "\"entries\":" + String(entries) + ",";
  json += "\"exits\":" + String(exits) + ",";
  json += "\"occupancy\":" + String(occupancy) + ",";
  json += "\"maxCapacity\":" + String(MAX_CAPACITY) + ",";
  json += "\"full\":" + String(roomFull ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleReset() {
  entries = 0; exits = 0; occupancy = 0; roomFull = false;
  saveCounters();
  refreshOLED();
  server.send(200, "text/plain", "OK");
}

void handleLogJson() {
#if ENABLE_LOCAL_LOG
  server.send(200, "application/json", readRecentLogAsJson(LOG_JSON_MAX_ROWS));
#else
  server.send(200, "application/json", "[]");
#endif
}

void handleExportCsv() {
#if ENABLE_LOCAL_LOG
  File f = LittleFS.open(LOG_FILE_PATH, "r");
  if (!f) { server.send(404, "text/plain", "No log file."); return; }
  server.streamFile(f, "text/csv");
  f.close();
#else
  server.send(404, "text/plain", "Local logging disabled.");
#endif
}

void handleClearLog() {
#if ENABLE_LOCAL_LOG
  clearLocalLog();
  server.send(200, "text/plain", "OK");
#else
  server.send(404, "text/plain", "Local logging disabled.");
#endif
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/reset", handleReset);
  server.on("/log", handleLogJson);
  server.on("/export", handleExportCsv);
  server.on("/clearlog", handleClearLog);
  server.begin();
  Serial.println(F("Web server started."));
}

#endif // ENABLE_WIFI_DASHBOARD

/* ============================== OPTIONAL: THINGSPEAK UPLOAD ============================== */
#if ENABLE_THINGSPEAK

void pushToThingSpeak() {
  HTTPClient http;
  String url = "http://api.thingspeak.com/update?api_key=" + String(THINGSPEAK_API_KEY) +
               "&field1=" + String(entries) +
               "&field2=" + String(exits) +
               "&field3=" + String(occupancy);
  http.begin(url);
  int code = http.GET();
  Serial.print(F("ThingSpeak upload -> HTTP "));
  Serial.println(code);
  http.end();
}

#endif // ENABLE_THINGSPEAK
