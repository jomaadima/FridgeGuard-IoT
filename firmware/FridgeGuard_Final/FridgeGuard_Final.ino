// ============================================================
//  FridgeGuard IoT — Final Sketch
//
//  Includes:
//    - WiFi connection + auto-reconnect
//    - Low-power WiFi fix for ESP32-C3 (see note near connectWiFi())
//    - NTP time sync
//    - DS18B20 temperature sensor
//    - Reed switch door detection with stable polling debounce
//    - OLED 6-screen dashboard:
//        0) Door icon + NTP clock only
//        1) Live Status (door/temp/wifi)
//        2) Today's Stats
//        3) Last Events
//        4) Alert History
//        5) System Info
//    - Telegram alerts for door / temperature / daily summary
//    - MAINTENANCE MODE (replaces the old interactive door-snooze flow):
//        * Hold BOOT button 3 seconds to ENTER maintenance mode.
//          -> All door/temp alerting and stats logging pauses.
//          -> OLED switches to a dedicated countdown screen.
//          -> Telegram sends a message with duration buttons
//             (10/20/30/60 min) and "End Maintenance Now".
//          -> You can also reply with a duration text:
//             60m, 1h40m, 2h, or plain 60 = minutes.
//          -> Default duration if you do nothing: 20 minutes.
//        * EXIT happens three ways:
//          1. Hold BOOT 3 seconds again -> Telegram asks Yes/No to end.
//          2. The countdown reaches zero -> ends automatically.
//          3. Tap "End Maintenance Now" on the original Telegram
//             message at any time, from anywhere.
//        * Maintenance timing uses millis(), NOT NTP epoch time, so it
//          still works correctly even on days NTP fails to sync.
//    - CLOUD SYNC (Firebase Firestore, free Spark plan):
//        * Live status uploaded every 12 seconds, plus instantly on
//          every door-close event.
//        * Each physical board uploads to its own document, keyed by
//          deviceId, so multiple boards never overwrite each other.
//    - DAILY STATISTICS:
//        * Accumulated in RAM all day (temp sum/min/max, openings,
//          total + longest open time, total thermal delta).
//        * Written to Firestore at day rollover under yesterday's date.
//    - WEEKLY ROLLING SUMMARY:
//        * 7-day rolling buffer of avg temperature, door openings, and
//          avg thermal delta, shifted forward at each day rollover.
//        * Uploaded as a single overwritten document to
//          weekly_summary/{deviceId} for the app's history view.
//    - FLASH PERSISTENCE (ESP32 NVS via Preferences library):
//        * Daily stats and the full weekly rolling buffer are saved
//          to flash so a power loss or reboot never loses same-day
//          or same-week data.
//
//  Door alert behavior:
//    - Alert #1 after door is open for 60 seconds
//    - Then repeats every 5 minutes while it stays open
//    - Paused entirely while Maintenance Mode is active
//
//  Temperature alert behavior:
//    - Temp alert when temp > 30C
//    - Repeats every 10 minutes while temp stays > 30C (was 5 min)
//    - 1C hysteresis before re-arming
//    - Paused entirely while Maintenance Mode is active
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>
#include <Preferences.h>

#include "secrets.h"
// ^ Copy secrets_template.h -> secrets.h in this folder and fill in
//   your own WiFi, Telegram, and Firebase values before uploading. See README.md.

// Unique ID for THIS physical board, generated in setup() from its
// built-in factory chip ID. This lets multiple people run the exact
// same code on their own boards without overwriting each other's data
// — each board gets its own private Firestore document automatically.
String deviceId;

// Firestore REST API endpoint — built in setup() once deviceId is known
String firestoreURL;

// ---------- Pin Definitions ----------
#define LED_PIN    8   // Onboard LED heartbeat
#define TEMP_PIN   4   // DS18B20 data wire
#define DOOR_PIN   3   // Reed switch
#define BUTTON_PIN 9   // BOOT button for screen cycling / maintenance mode

// ---------- Temperature Sensor ----------
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);
float currentTemp = 0.0;

// ---------- OLED Display ----------
// OLED wiring used by your code: SCL = GPIO 6, SDA = GPIO 5
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);

// ---------- Door / Reed Switch State ----------
// Normal wiring:
// GPIO3 ---- reed switch ---- GND
// With INPUT_PULLUP:
// HIGH = door open
// LOW  = door closed
#define DOOR_OPEN_LEVEL HIGH

bool doorIsOpen = false;
bool lastStableDoorState = false;
bool lastRawDoorState = false;

unsigned long lastDoorDebounceTime = 0;
const unsigned long DOOR_DEBOUNCE_DELAY = 80;

unsigned long doorOpenTime = 0;

// ---------- Button / Screen State Machine ----------
// Screens, in cycling order:
#define SCREEN_ICON_TIME   0   // door icon + NTP clock only
#define SCREEN_LIVE_STATUS 1
#define SCREEN_STATS       2
#define SCREEN_EVENTS      3
#define SCREEN_ALERTS      4
#define SCREEN_SYSTEM      5
#define SCREEN_COUNT       6

int currentScreen = SCREEN_ICON_TIME;

// Short-press / long-press detection (button uses CHANGE interrupt now,
// so we can measure how long it was held).
#define LONG_PRESS_MS       3000UL  // hold this long to trigger maintenance mode
#define BUTTON_DEBOUNCE_MS  50UL    // ignore edges closer together than this

volatile bool buttonPressed     = false;  // short-press flag (screen cycling)
volatile bool longPressDetected = false;  // long-press flag (maintenance mode)
volatile bool buttonCurrentlyDown = false;
volatile unsigned long buttonDownTime    = 0;
volatile unsigned long lastButtonEdgeTime = 0;

// ---------- Daily Statistics ----------
int totalOpenings = 0;
unsigned long totalOpenSeconds = 0;
unsigned long longestOpening = 0;

// Running temperature average (accumulated each temp reading, divided at midnight)
float tempSum = 0.0;
int   tempSampleCount = 0;
float tempMinToday = 999.0;
float tempMaxToday = -999.0;

// Thermal impact tracking — temperature snapshot at door open,
// compared to temperature at door close to measure each opening's
// thermal impact on the fridge.
float tempAtDoorOpen   = 0.0;   // snapshotted when door opens
float lastTempDelta    = 0.0;   // most recent opening's impact
float totalTempDelta   = 0.0;   // running sum for daily average

struct DoorEvent {
  char time[12];
  unsigned long duration;
  float tempDelta;    // how much temp changed during this opening
};

DoorEvent lastEvents[3] = {
  {"--:--", 0, 0.0},
  {"--:--", 0, 0.0},
  {"--:--", 0, 0.0}
};

String lastAlertMsg = "None yet";
char lastAlertTime[12] = "--:--";

// ---------- Alert Thresholds & Tracking ----------
#define DOOR_ALERT_SECONDS       60
#define DOOR_ALERT_REPEAT_MS     120000UL  // 2 minutes

#define TEMP_THRESHOLD_C         8.0
#define TEMP_HYSTERESIS_C        1.0
#define TEMP_ALERT_REPEAT_MS     300000UL  // 5 minutes 

#define SUMMARY_HOUR             0

bool tempAlertSent = false;
unsigned long lastTempAlertTime = 0;
bool summarySentToday = false;

// Door alert tracking
int doorAlertCount = 0;
unsigned long lastDoorAlertTime = 0;

// ---------- Maintenance Mode ----------
#define DEFAULT_MAINTENANCE_MS (20UL * 60UL * 1000UL)  // 20 minutes default

bool maintenanceMode = false;
unsigned long maintenanceStartTime = 0;
unsigned long maintenanceDurationMs = 0;

// ---------- Telegram Polling ----------
unsigned long lastTelegramPoll = 0;
#define TELEGRAM_POLL_INTERVAL_MS 3000UL

long telegramUpdateOffset = 0;

// ---------- Time Sync ----------
bool ntpSynced = false;

// ---------- Non-Blocking Timers ----------
unsigned long lastTempRead = 0;
#define TEMP_READ_INTERVAL_MS 2000UL

unsigned long lastWifiCheck = 0;
#define WIFI_RECONNECT_INTERVAL_MS 30000UL

// WiFi/Power status tracking
// IMPORTANT: the ESP32 cannot send Telegram while power/WiFi is already lost.
// So we send an ONLINE message on boot and a RESTORED message after it reconnects.
bool wifiWasConnected = false;
unsigned long wifiLostAt = 0;

unsigned long lastScreenRefresh = 0;
#define SCREEN_REFRESH_INTERVAL_MS 200UL

unsigned long lastHeartbeat = 0;
#define HEARTBEAT_INTERVAL_MS 1000UL

unsigned long lastCloudUpload = 0;
#define CLOUD_UPLOAD_INTERVAL_MS 12000UL  // Upload to Firestore every 12 seconds
                                           // (12s × 2 boards = 14,400 writes/day —
                                           //  72% of Firestore free-tier 20,000 limit)

unsigned long lastCommandCheck = 0;
#define COMMAND_CHECK_INTERVAL_MS 5000UL  // Check for app commands every 5 seconds
                                           // This is a read, not a write — reads are
                                           // 50,000/day free, so 17,280/day is fine

bool ledState = false;

// ---------- Persistent Daily Stats (ESP32 flash / NVS) ----------
// This saves only the daily counters/data, NOT the uploaded program.
// Program code stays safe. Only keys inside the "fg_stats" namespace are updated/reset.
Preferences statsPrefs;
String activeStatsDate = "";

bool pendingSummaryWaiting = false;

// ---------- Weekly Rolling Buffer ----------
// Stores the last 7 days of daily stats in RAM + flash.
// Index 0 = most recent completed day, index 6 = oldest.
// Used to calculate weekly averages uploaded to Firestore
// as a single readable document for the app.
#define WEEKLY_DAYS 7
float weeklyAvgTemp[WEEKLY_DAYS]  = {0};
int   weeklyOpenings[WEEKLY_DAYS] = {0};
float weeklyAvgDelta[WEEKLY_DAYS] = {0};
int   weeklyDaysFilled = 0;

struct DailyStatsSnapshot {
  String date;
  int totalOpenings;
  unsigned long totalOpenSeconds;
  unsigned long longestOpening;
  float tempSum;
  int tempSampleCount;
  float tempMinToday;
  float tempMaxToday;
  float totalTempDelta;
  float lastTempDelta;
  DoorEvent events[3];
};

unsigned long lastStatsAutosave = 0;
#define STATS_AUTOSAVE_INTERVAL_MS 60000UL  // backup to flash once per minute
unsigned long lastPendingSummaryAttempt = 0;
#define PENDING_SUMMARY_RETRY_MS 60000UL  // avoid Telegram spam if server/Firestore fails

// ============================================================
//  BUTTON INTERRUPT — now detects BOTH short and long presses
// ============================================================
// We trigger on CHANGE (both edges) so we can time how long the
// button was held between press and release.
void IRAM_ATTR buttonISR() {
  unsigned long now = millis();

  if (now - lastButtonEdgeTime < BUTTON_DEBOUNCE_MS) {
    return;  // bouncing contact, ignore this edge
  }
  lastButtonEdgeTime = now;

  bool isDown = (digitalRead(BUTTON_PIN) == LOW);  // INPUT_PULLUP: pressed = LOW

  if (isDown && !buttonCurrentlyDown) {
    // Press just started
    buttonCurrentlyDown = true;
    buttonDownTime = now;
  } else if (!isDown && buttonCurrentlyDown) {
    // Just released — decide short vs long press
    buttonCurrentlyDown = false;
    unsigned long heldFor = now - buttonDownTime;

    if (heldFor >= LONG_PRESS_MS) {
      longPressDetected = true;
    } else {
      buttonPressed = true;
    }
  }
}

// ============================================================
//  WIFI — LOW POWER FIX
// ============================================================
// NOTE FOR DIMA: WIFI_POWER_8_5dBm reduces transmit strength. This is
// usually added to avoid brownout resets on a weak USB power supply,
// but it also weakens range/reliability. If you raise this value
// (e.g. WIFI_POWER_13dBm or remove the line for default ~19.5dBm) and
// the board starts randomly rebooting, that confirms a power-supply
// issue — fix it with a better cable/adapter instead of lowering this
// value further. If raising it does NOT cause reboots, keep it raised.
void prepareWiFiLowPower() {
  WiFi.mode(WIFI_STA);

  // Do not save WiFi credentials to flash repeatedly
  WiFi.persistent(false);

  // Keep WiFi awake/stable
  WiFi.setSleep(false);

  // IMPORTANT FIX FOR YOUR ESP32-C3 (see note above)
  WiFi.setTxPower(WIFI_POWER_13dBm);
}

void connectWiFi() {
  // Fully power off the WiFi radio before reconfiguring. Just calling
  // disconnect() without wifioff=true can leave the driver's internal
  // state machine thinking it's still "connecting" from the previous
  // attempt, which causes a "sta is connecting, cannot set config"
  // error on the next try — this matters a lot here since connectWiFi()
  // gets called again automatically every time WiFi drops.
  WiFi.disconnect(true, true);  // true = turn radio off, true = erase old AP info
  delay(300);

  WiFi.mode(WIFI_STA);
  delay(200);

  prepareWiFiLowPower();
  delay(1500);

  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi CONNECTED!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    Serial.print("Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("WiFi connect attempt timed out — will retry in loop().");
  }
}

// ============================================================
//  NTP TIME
// ============================================================
void syncNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("NTP skipped — WiFi not connected.");
    ntpSynced = false;
    return;
  }

  // Lebanon time = UTC +3
  configTime(3 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  Serial.print("Waiting for NTP time sync");

  struct tm timeInfo;
  unsigned long ntpStart = millis();

  while (!getLocalTime(&timeInfo) && millis() - ntpStart < 15000) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();

  if (getLocalTime(&timeInfo)) {
    ntpSynced = true;
    Serial.println("NTP synced!");
    Serial.printf("Current time: %02d:%02d:%02d  %02d/%02d/%04d\n",
                  timeInfo.tm_hour,
                  timeInfo.tm_min,
                  timeInfo.tm_sec,
                  timeInfo.tm_mday,
                  timeInfo.tm_mon + 1,
                  timeInfo.tm_year + 1900);
  } else {
    ntpSynced = false;
    Serial.println("NTP sync FAILED — check WiFi/internet connection.");
  }
}

// ============================================================
//  TELEGRAM HELPERS
// ============================================================
String urlEncode(String str) {
  String encoded = "";

  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);

    if (isalnum((unsigned char)c)) {
      encoded += c;
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }

  return encoded;
}

String jsonExtractString(String src, String key, int startIndex = 0) {
  String pattern = "\"" + key + "\":\"";
  int start = src.indexOf(pattern, startIndex);

  if (start < 0) return "";

  start += pattern.length();
  int end = src.indexOf("\"", start);

  if (end < 0) return "";

  return src.substring(start, end);
}

long jsonExtractLong(String src, String key, int startIndex = 0) {
  String pattern = "\"" + key + "\":";
  int start = src.indexOf(pattern, startIndex);

  if (start < 0) return -1;

  start += pattern.length();
  int end = start;

  while (end < src.length() && isdigit(src.charAt(end))) {
    end++;
  }

  if (end == start) return -1;

  return src.substring(start, end).toInt();
}

void updateLastAlertTime();
void saveCurrentStatsToFlash();
void loadCurrentStatsFromFlash();
void checkDailyDateRollover();

bool sendTelegram(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot send Telegram — WiFi not connected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(4000);

  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/sendMessage?chat_id=";
  url += CHAT_ID;
  url += "&text=";
  url += urlEncode(message);

  http.begin(client, url);

  int httpCode = http.GET();

  bool ok = (httpCode == 200);

  if (ok) {
    Serial.println("Telegram sent successfully");
  } else {
    Serial.print("Telegram failed, code: ");
    Serial.println(httpCode);
    Serial.println("401 = wrong bot token");
    Serial.println("400 = wrong chat id / message format");
  }

  http.end();
  return ok;
}

void sendTelegramWithKeyboard(String message, String keyboardJson) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot send Telegram buttons — WiFi not connected");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(4000);

  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/sendMessage?chat_id=";
  url += CHAT_ID;
  url += "&text=";
  url += urlEncode(message);
  url += "&reply_markup=";
  url += urlEncode(keyboardJson);

  http.begin(client, url);

  int httpCode = http.GET();

  if (httpCode == 200) {
    Serial.println("Telegram interactive message sent");
  } else {
    Serial.print("Telegram button message failed, code: ");
    Serial.println(httpCode);
  }

  http.end();
}

void answerCallbackQuery(String callbackId, String text) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(4000);

  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/answerCallbackQuery?callback_query_id=";
  url += callbackId;
  url += "&text=";
  url += urlEncode(text);

  http.begin(client, url);
  http.GET();
  http.end();
}

// ============================================================
//  FIREBASE / FIRESTORE — CLOUD UPLOAD
// ============================================================
void sendToFirestore() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot upload — WiFi not connected");
    return;
  }

  HTTPClient http;
  http.begin(firestoreURL);
  http.addHeader("Content-Type", "application/json");

  // Firestore's REST API requires this specific nested "fields" format.
  // Each value must declare its type: stringValue, doubleValue, integerValue
  String json = "{\"fields\": {";
  json += "\"doorStatus\": {\"stringValue\": \"" + String(doorIsOpen ? "OPEN" : "CLOSED") + "\"},";
  json += "\"temperature\": {\"doubleValue\": " + String(currentTemp, 1) + "},";
  json += "\"wifiSignal\": {\"integerValue\": " + String(WiFi.RSSI()) + "},";
  json += "\"totalOpenings\": {\"integerValue\": " + String(totalOpenings) + "},";
  json += "\"totalOpenSeconds\": {\"integerValue\": " + String(totalOpenSeconds) + "},";
  json += "\"longestOpening\": {\"integerValue\": " + String(longestOpening) + "},";
  json += "\"lastAlertMessage\": {\"stringValue\": \"" + lastAlertMsg + "\"},";
  json += "\"lastTempDelta\": {\"doubleValue\": " + String(lastTempDelta, 1) + "},";

  // Maintenance status fields — read by the app to show status + countdown
  long remainingSec = 0;
  if (maintenanceMode) {
    long elapsed = (long)(millis() - maintenanceStartTime);
    long remaining = (long)maintenanceDurationMs - elapsed;
    remainingSec = (remaining > 0) ? remaining / 1000 : 0;
  }
  json += "\"maintenanceActive\": {\"booleanValue\": " + String(maintenanceMode ? "true" : "false") + "},";
  json += "\"maintenanceSecondsLeft\": {\"integerValue\": " + String(remainingSec) + "},";

  // Command field — written by app, cleared by ESP32 after acting on it.
  // We include it in PATCH with its current value so we never accidentally
  // overwrite a pending command that arrived between our polls.
  // We use updateMask in clearFirestoreCommand() to only touch this field,
  // so leaving it out of the regular PATCH is safe — Firestore PATCH with
  // no updateMask only updates fields explicitly listed, not the whole doc.
  json += "\"lastUpdated\": {\"stringValue\": \"" + String(millis() / 1000) + "s\"},";
  json += "\"deviceId\": {\"stringValue\": \"" + deviceId + "\"}";
  json += "}}";

  // PATCH = update this exact document, creating it the first time
  int httpCode = http.PATCH(json);

  if (httpCode == 200) {
    Serial.println("Firestore updated successfully");
  } else {
    Serial.print("Firestore upload failed, code: ");
    Serial.println(httpCode);
    // 400 = malformed JSON. 403 = wrong API key, wrong project ID, or test mode expired.
  }

  http.end();
}

// ============================================================
//  FIREBASE / FIRESTORE — DAILY STATS WRITE
//  Writes one permanent document per day to:
//  daily_stats/{deviceId}/days/{YYYY-MM-DD}
//  This collection accumulates over time — one doc per day,
//  never overwritten — enabling weekly/monthly history in the app.
// ============================================================
void sendToDailyStats() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot write daily stats — WiFi not connected");
    return;
  }

  struct tm t;
  if (!getLocalTime(&t)) {
    Serial.println("Cannot write daily stats — NTP time not ready");
    return;
  }

  // Build the date string for today's document name: YYYY-MM-DD
  char dateStr[12];
  snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

  // Average temperature for the day
  float avgTemp = (tempSampleCount > 0) ? (tempSum / tempSampleCount) : 0.0;

  // Average thermal delta per opening
  float avgTempDelta = (totalOpenings > 0) ? (totalTempDelta / totalOpenings) : 0.0;

  // Firestore path for this day's document
  String statsURL = "https://firestore.googleapis.com/v1/projects/"
                    + String(FIREBASE_PROJECT_ID)
                    + "/databases/(default)/documents/daily_stats/"
                    + deviceId + "/days/" + String(dateStr)
                    + "?key=" + String(FIREBASE_API_KEY);

  HTTPClient http;
  http.begin(statsURL);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"fields\": {";
  json += "\"date\": {\"stringValue\": \"" + String(dateStr) + "\"},";
  json += "\"deviceId\": {\"stringValue\": \"" + deviceId + "\"},";
  json += "\"totalOpenings\": {\"integerValue\": " + String(totalOpenings) + "},";
  json += "\"totalOpenSeconds\": {\"integerValue\": " + String(totalOpenSeconds) + "},";
  json += "\"longestOpening\": {\"integerValue\": " + String(longestOpening) + "},";
  json += "\"avgTemperature\": {\"doubleValue\": " + String(avgTemp, 1) + "},";
  json += "\"minTemperature\": {\"doubleValue\": " + String(tempMinToday, 1) + "},";
  json += "\"maxTemperature\": {\"doubleValue\": " + String(tempMaxToday, 1) + "},";
  json += "\"avgTempDelta\": {\"doubleValue\": " + String(avgTempDelta, 1) + "}";
  json += "}}";

  int httpCode = http.PATCH(json);

  if (httpCode == 200) {
    Serial.print("Daily stats written for ");
    Serial.println(dateStr);
  } else {
    Serial.print("Daily stats write failed, code: ");
    Serial.println(httpCode);
  }

  http.end();
}

// ============================================================
//  FIREBASE / FIRESTORE — ALERTS HISTORY
//  Appends one document per alert to:
//  alerts/{deviceId}/history/{timestamp}
//  This collection grows over time — one doc per alert event —
//  enabling the scrollable alerts history list in the app.
//
//  category values (used by app for icon/color):
//    "door"        → red door icon
//    "temperature" → amber thermometer icon
//    "maintenance" → teal tool icon
//    "system"      → teal wifi icon
// ============================================================
void logAlertToFirestore(String category, String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  // Build a timestamp string for both the document name and the
  // stored field — uses NTP time if available, millis() as fallback
  char timestampStr[24];
  struct tm t;
  if (getLocalTime(&t)) {
    snprintf(timestampStr, sizeof(timestampStr), "%04d-%02d-%02dT%02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    snprintf(timestampStr, sizeof(timestampStr), "boot+%lus", millis() / 1000);
  }

  // Use timestamp as document name — ensures chronological order
  // and prevents duplicate writes for the same alert
  String alertURL = "https://firestore.googleapis.com/v1/projects/"
                    + String(FIREBASE_PROJECT_ID)
                    + "/databases/(default)/documents/alerts/"
                    + deviceId + "/history/" + String(timestampStr)
                    + "?key=" + String(FIREBASE_API_KEY);

  HTTPClient http;
  http.begin(alertURL);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"fields\": {";
  json += "\"category\": {\"stringValue\": \"" + category + "\"},";
  json += "\"message\": {\"stringValue\": \"" + message + "\"},";
  json += "\"timestamp\": {\"stringValue\": \"" + String(timestampStr) + "\"},";
  json += "\"deviceId\": {\"stringValue\": \"" + deviceId + "\"}";
  json += "}}";

  int httpCode = http.PATCH(json);

  if (httpCode == 200) {
    Serial.print("Alert logged: [");
    Serial.print(category);
    Serial.print("] ");
    Serial.println(message);
  } else {
    Serial.print("Alert log failed, code: ");
    Serial.println(httpCode);
  }

  http.end();
}


//  The app writes "maintenanceCommand" to the same fridge_data
//  document the ESP32 already uploads to. We GET that document
//  every 5 seconds, parse the command field, act on it, then
//  immediately clear it back to "" so it only fires once.
//
//  Command strings the app can write:
//    "START_10"  → enter maintenance for 10 minutes
//    "START_20"  → enter maintenance for 20 minutes
//    "START_30"  → enter maintenance for 30 minutes
//    "START_60"  → enter maintenance for 60 minutes
//    "START_N"   → enter maintenance for N minutes (any number)
//    "END"       → exit maintenance immediately
//    ""          → idle, do nothing
// ============================================================
void clearFirestoreCommand() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(firestoreURL + "&updateMask.fieldPaths=maintenanceCommand");
  http.addHeader("Content-Type", "application/json");

  String json = "{\"fields\": {";
  json += "\"maintenanceCommand\": {\"stringValue\": \"\"}";
  json += "}}";

  http.PATCH(json);
  http.end();
}

void checkFirestoreCommand() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(firestoreURL);

  int httpCode = http.GET();

  if (httpCode != 200) {
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  // Pull out the maintenanceCommand field value using our existing
  // JSON extractor — same pattern used for Telegram updates.
  // Look for: "maintenanceCommand":{"stringValue":"START_20"}
  String pattern = "\"maintenanceCommand\"";
  int pos = payload.indexOf(pattern);
  if (pos < 0) return;  // field not yet in document (first boot)

  String cmd = jsonExtractString(payload, "stringValue", pos);
  cmd.trim();

  if (cmd.length() == 0) return;  // empty = no pending command

  Serial.print("App command received: ");
  Serial.println(cmd);

  // Clear the command immediately before acting, so a slow action
  // doesn't let the same command fire twice on the next poll.
  clearFirestoreCommand();

  if (cmd == "END") {
    if (maintenanceMode) {
      exitMaintenanceMode(true);
      sendTelegram("Maintenance Mode ended remotely via app.");
    }
    return;
  }

  if (cmd.startsWith("START_")) {
    String minutesPart = cmd.substring(6);  // everything after "START_"
    int minutes = minutesPart.toInt();

    if (minutes > 0 && minutes <= 240) {
      if (maintenanceMode) {
        // Already in maintenance — just update the duration
        setMaintenanceDurationMinutes(minutes);
        String msg = "Maintenance duration updated to " + String(minutes) + " minutes via app.";
        sendTelegram(msg);
        Serial.println(msg);
      } else {
        // Enter maintenance with the requested duration
        enterMaintenanceMode();
        setMaintenanceDurationMinutes(minutes);
        String msg = "Maintenance Mode started from app for " + String(minutes) + " minutes.";
        sendTelegram(msg);
        Serial.println(msg);
      }
    }
  }
}

void setMaintenanceDurationMinutes(int minutes) {
  // Keep the same start time — just change the total window length.
  // Remaining time is always recalculated from (start + duration) - now.
  maintenanceDurationMs = (unsigned long)minutes * 60000UL;
}

String formatMinutesPretty(int totalMinutes) {
  int hours = totalMinutes / 60;
  int minutes = totalMinutes % 60;

  String result = "";

  if (hours > 0) {
    result += String(hours);
    result += "h";
  }

  if (minutes > 0) {
    if (result.length() > 0) result += " ";
    result += String(minutes);
    result += "m";
  }

  if (result.length() == 0) {
    result = "0m";
  }

  return result;
}

int parseMaintenanceDurationText(String input) {
  input.trim();
  input.toLowerCase();
  input.replace(" ", "");

  if (input.length() == 0) return -1;

  // Keep old behavior too: plain 60 means 60 minutes.
  bool allDigits = true;

  for (int i = 0; i < input.length(); i++) {
    if (!isdigit(input.charAt(i))) {
      allDigits = false;
      break;
    }
  }

  if (allDigits) {
    return input.toInt();
  }

  int totalMinutes = 0;
  int i = 0;
  bool foundUnit = false;

  while (i < input.length()) {
    if (!isdigit(input.charAt(i))) {
      return -1;
    }

    int numberStart = i;

    while (i < input.length() && isdigit(input.charAt(i))) {
      i++;
    }

    int value = input.substring(numberStart, i).toInt();

    if (i >= input.length()) {
      return -1;  // number without h/m at the end
    }

    char unit = input.charAt(i);
    i++;

    if (unit == 'h') {
      totalMinutes += value * 60;
      foundUnit = true;
    } else if (unit == 'm') {
      totalMinutes += value;
      foundUnit = true;
    } else {
      return -1;
    }
  }

  if (!foundUnit) return -1;

  return totalMinutes;
}

void sendTelegramMaintenanceStart() {
  String message = "Maintenance Mode activated.\n";
  message += "Door & temperature alerts are paused for 20 minutes by default.\n\n";
  message += "To change the duration, tap a button OR send a message in this format:\n";
  message += "60m = 60 minutes\n";
  message += "1h40m = 1 hour 40 minutes\n";
  message += "2h = 2 hours\n";
  message += "60 = 60 minutes too\n";
  message += "Limit: 1m to 4h.\n\n";
  message += "Tap 'End Maintenance Now' anytime to resume immediately.";

  String keyboard = "{\"inline_keyboard\":[";
  keyboard += "[{\"text\":\"10 min\",\"callback_data\":\"maint_10\"},";
  keyboard += "{\"text\":\"20 min\",\"callback_data\":\"maint_20\"}],";
  keyboard += "[{\"text\":\"30 min\",\"callback_data\":\"maint_30\"},";
  keyboard += "{\"text\":\"60 min\",\"callback_data\":\"maint_60\"}],";
  keyboard += "[{\"text\":\"End Maintenance Now\",\"callback_data\":\"maint_end\"}]";
  keyboard += "]}";

  sendTelegramWithKeyboard(message, keyboard);

  lastAlertMsg = "Maint. started";
  updateLastAlertTime();
}

void requestMaintenanceExitConfirmation() {
  String message = "Maintenance Mode is active. Do you want to end it now?";

  String keyboard = "{\"inline_keyboard\":[";
  keyboard += "[{\"text\":\"Yes, end it\",\"callback_data\":\"maint_exit_yes\"},";
  keyboard += "{\"text\":\"No, continue\",\"callback_data\":\"maint_exit_no\"}]";
  keyboard += "]}";

  sendTelegramWithKeyboard(message, keyboard);

  Serial.println("Maintenance exit confirmation requested via Telegram");
}

void resetDoorAlertState() {
  doorAlertCount = 0;
  lastDoorAlertTime = 0;
}

void enterMaintenanceMode() {
  maintenanceMode = true;
  maintenanceStartTime = millis();
  maintenanceDurationMs = DEFAULT_MAINTENANCE_MS;

  resetDoorAlertState();
  tempAlertSent = false;
  lastTempAlertTime = 0;

  sendTelegramMaintenanceStart();
  logAlertToFirestore("maintenance", "Maintenance Mode activated.");

  Serial.println("Entered MAINTENANCE MODE");
}

void exitMaintenanceMode(bool notify) {
  maintenanceMode = false;
  maintenanceDurationMs = 0;

  resetDoorAlertState();
  tempAlertSent = false;
  lastTempAlertTime = 0;

  // If the door is still open when maintenance ends, restart its
  // 60-second grace window fresh instead of alerting immediately.
  if (doorIsOpen) {
    doorOpenTime = millis();
  }

  if (notify) {
    sendTelegram("Maintenance Mode ended. Resuming normal monitoring.");
    logAlertToFirestore("maintenance", "Maintenance Mode ended.");
    lastAlertMsg = "Maint. ended";
    updateLastAlertTime();
  }

  currentScreen = SCREEN_ICON_TIME;

  Serial.println("Exited MAINTENANCE MODE");
}

void checkMaintenanceExpiry() {
  if (!maintenanceMode) return;

  if (millis() - maintenanceStartTime >= maintenanceDurationMs) {
    exitMaintenanceMode(true);
  }
}

// ============================================================
//  WIFI / POWER STATUS TELEGRAM MESSAGES
// ============================================================
String formatMillisPretty(unsigned long ms) {
  unsigned long totalSeconds = ms / 1000UL;
  unsigned long totalMinutes = totalSeconds / 60UL;
  unsigned long hours = totalMinutes / 60UL;
  unsigned long minutes = totalMinutes % 60UL;
  unsigned long seconds = totalSeconds % 60UL;

  String result = "";

  if (hours > 0) {
    result += String(hours);
    result += "h ";
  }

  if (minutes > 0 || hours > 0) {
    result += String(minutes);
    result += "m";
  } else {
    result += String(seconds);
    result += "s";
  }

  return result;
}

void sendTelegramOnlineStatus() {
  String message = "FridgeGuard is ONLINE.\n";
  message += "Power/WiFi connected.\n";
  message += "Monitoring started.";

  sendTelegram(message);
  logAlertToFirestore("system", "FridgeGuard back online.");

  lastAlertMsg = "ESP online";
  updateLastAlertTime();
}

void sendTelegramConnectionRestored(unsigned long offlineMs) {
  String message = "FridgeGuard connection restored.\n";
  message += "It could not reach Telegram for about ";
  message += formatMillisPretty(offlineMs);
  message += ".\n";
  message += "Monitoring continues.";

  sendTelegram(message);
  logAlertToFirestore("system", "Connection restored after " + formatMillisPretty(offlineMs) + ".");

  lastAlertMsg = "Conn. restored";
  updateLastAlertTime();
}

// ============================================================
//  TELEGRAM RECEIVING / INTERACTION
// ============================================================
String telegramGetUpdates() {
  if (WiFi.status() != WL_CONNECTED) return "";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(5000);

  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/getUpdates?timeout=0";

  if (telegramUpdateOffset > 0) {
    url += "&offset=";
    url += String(telegramUpdateOffset);
  }

  http.begin(client, url);

  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.print("getUpdates failed, code: ");
    Serial.println(httpCode);
    http.end();
    return "";
  }

  String payload = http.getString();
  http.end();

  return payload;
}

void clearOldTelegramUpdates() {
  String payload = telegramGetUpdates();

  if (payload.length() == 0) return;

  long maxUpdateId = -1;
  int index = 0;

  while (true) {
    int pos = payload.indexOf("\"update_id\":", index);

    if (pos < 0) break;

    long id = jsonExtractLong(payload, "update_id", pos);

    if (id > maxUpdateId) {
      maxUpdateId = id;
    }

    index = pos + 12;
  }

  if (maxUpdateId >= 0) {
    telegramUpdateOffset = maxUpdateId + 1;
    Serial.print("Telegram updates cleared. Next offset: ");
    Serial.println(telegramUpdateOffset);
  }
}

void handleTelegramCallback(String data, String callbackId) {
  Serial.print("Telegram callback: ");
  Serial.println(data);

  if (data == "maint_10") {
    answerCallbackQuery(callbackId, "Set to 10 minutes");
    setMaintenanceDurationMinutes(10);
    sendTelegram("Maintenance duration set to 10 minutes.");
    lastAlertMsg = "Maint. 10 min";
    updateLastAlertTime();

  } else if (data == "maint_20") {
    answerCallbackQuery(callbackId, "Set to 20 minutes");
    setMaintenanceDurationMinutes(20);
    sendTelegram("Maintenance duration set to 20 minutes.");
    lastAlertMsg = "Maint. 20 min";
    updateLastAlertTime();

  } else if (data == "maint_30") {
    answerCallbackQuery(callbackId, "Set to 30 minutes");
    setMaintenanceDurationMinutes(30);
    sendTelegram("Maintenance duration set to 30 minutes.");
    lastAlertMsg = "Maint. 30 min";
    updateLastAlertTime();

  } else if (data == "maint_60") {
    answerCallbackQuery(callbackId, "Set to 60 minutes");
    setMaintenanceDurationMinutes(60);
    sendTelegram("Maintenance duration set to 60 minutes.");
    lastAlertMsg = "Maint. 60 min";
    updateLastAlertTime();

  } else if (data == "maint_end") {
    answerCallbackQuery(callbackId, "Ending maintenance now");
    exitMaintenanceMode(true);

  } else if (data == "maint_exit_yes") {
    answerCallbackQuery(callbackId, "Ending maintenance");
    exitMaintenanceMode(true);

  } else if (data == "maint_exit_no") {
    answerCallbackQuery(callbackId, "Continuing maintenance");
    sendTelegram("Okay, maintenance mode continues.");
  }
}

void handleTelegramText(String text) {
  Serial.print("Telegram text: ");
  Serial.println(text);

  if (!maintenanceMode) return;  // duration replies only matter during maintenance

  text.trim();

  if (text.length() == 0) return;

  int minutes = parseMaintenanceDurationText(text);

  if (minutes > 0 && minutes <= 240) {
    setMaintenanceDurationMinutes(minutes);

    String msg = "Maintenance duration set to ";
    msg += String(minutes);
    msg += " minutes";

    String pretty = formatMinutesPretty(minutes);
    if (pretty.length() > 0) {
      msg += " (";
      msg += pretty;
      msg += ")";
    }

    msg += ".";
    sendTelegram(msg);

    lastAlertMsg = "Maint. " + String(minutes) + " min";
    updateLastAlertTime();
  } else {
    String msg = "I did not understand that duration.\n";
    msg += "Use 1 minute to 4 hours.\n";
    msg += "Examples:\n";
    msg += "60m = 60 minutes\n";
    msg += "1h40m = 1 hour 40 minutes\n";
    msg += "2h = 2 hours\n";
    msg += "60 = 60 minutes";
    sendTelegram(msg);
  }
}

void processTelegramUpdates() {
  String payload = telegramGetUpdates();

  if (payload.length() == 0) return;

  int index = 0;

  while (true) {
    int updatePos = payload.indexOf("\"update_id\":", index);

    if (updatePos < 0) break;

    int nextUpdatePos = payload.indexOf("\"update_id\":", updatePos + 12);
    String oneUpdate;

    if (nextUpdatePos < 0) {
      oneUpdate = payload.substring(updatePos);
      index = payload.length();
    } else {
      oneUpdate = payload.substring(updatePos, nextUpdatePos);
      index = nextUpdatePos;
    }

    long updateId = jsonExtractLong(oneUpdate, "update_id");

    if (updateId >= 0 && updateId >= telegramUpdateOffset) {
      telegramUpdateOffset = updateId + 1;
    }

    int callbackPos = oneUpdate.indexOf("\"callback_query\"");

    if (callbackPos >= 0) {
      String callbackId = jsonExtractString(oneUpdate, "id", callbackPos);
      String data = jsonExtractString(oneUpdate, "data", callbackPos);

      if (callbackId.length() > 0 && data.length() > 0) {
        handleTelegramCallback(data, callbackId);
      }
    }

    int messagePos = oneUpdate.indexOf("\"message\"");

    if (messagePos >= 0) {
      String text = jsonExtractString(oneUpdate, "text", messagePos);

      if (text.length() > 0) {
        handleTelegramText(text);
      }
    }

    if (index >= payload.length()) break;
  }
}

// ============================================================
//  DOOR FUNCTIONS — STABLE POLLING DEBOUNCE
// ============================================================
bool readDoorOpenRaw() {
  return digitalRead(DOOR_PIN) == DOOR_OPEN_LEVEL;
}

void getShortTime(char* timeStr, size_t size) {
  struct tm t;

  if (getLocalTime(&t)) {
    snprintf(timeStr, size, "%02d:%02d", t.tm_hour, t.tm_min);
  } else {
    snprintf(timeStr, size, "--:--");
  }
}

void updateLastAlertTime() {
  getShortTime(lastAlertTime, sizeof(lastAlertTime));
}

void handleDoorChange(bool newDoorState) {
  doorIsOpen = newDoorState;

  char timeStr[12];
  getShortTime(timeStr, sizeof(timeStr));

  // While in Maintenance Mode: track open/close locally (so the grace
  // window is correct when maintenance ends) but do NOT touch stats
  // or alert counters, and don't log to the events list.
  if (maintenanceMode) {
    if (doorIsOpen) {
      doorOpenTime = millis();
      Serial.print("DOOR OPENED (maintenance — not logged) at ");
      Serial.println(timeStr);
    } else {
      Serial.print("DOOR CLOSED (maintenance — not logged) at ");
      Serial.println(timeStr);
    }
    resetDoorAlertState();
    return;
  }

  if (doorIsOpen) {
    doorOpenTime = millis();
    tempAtDoorOpen = currentTemp;  // snapshot temperature the moment door opens
    resetDoorAlertState();

    Serial.print("DOOR OPENED at ");
    Serial.print(timeStr);
    Serial.print(" — temp at open: ");
    Serial.print(tempAtDoorOpen, 1);
    Serial.println("C");
  } else {
    unsigned long duration = 0;

    if (doorOpenTime > 0) {
      duration = (millis() - doorOpenTime) / 1000;
    }

    // Thermal impact: how much did the temperature change while the door was open?
    // Positive = fridge warmed up (warm air got in — expected)
    // Negative = fridge cooled down (unusual, possible near freezer)
    // Near zero = door was opened and closed very quickly
    lastTempDelta = currentTemp - tempAtDoorOpen;
    totalTempDelta += lastTempDelta;

    totalOpenings++;
    totalOpenSeconds += duration;

    if (duration > longestOpening) {
      longestOpening = duration;
    }

    lastEvents[2] = lastEvents[1];
    lastEvents[1] = lastEvents[0];

    strncpy(lastEvents[0].time, timeStr, sizeof(lastEvents[0].time) - 1);
    lastEvents[0].time[sizeof(lastEvents[0].time) - 1] = '\0';
    lastEvents[0].duration = duration;
    lastEvents[0].tempDelta = lastTempDelta;

    resetDoorAlertState();

    Serial.print("DOOR CLOSED at ");
    Serial.print(timeStr);
    Serial.print(", open for ");
    Serial.print(duration);
    Serial.print("s, temp delta: ");
    Serial.print(lastTempDelta, 1);
    Serial.println("C");

    // Save important stats immediately after a completed opening.
    // This protects the day's stats if power cuts after the door closes.
    saveCurrentStatsToFlash();

    // Instant cloud update on door close, instead of waiting for the
    // 30-second timer — keeps the phone app feeling responsive.
    sendToFirestore();
  }
}

void updateDoor() {
  bool rawDoorState = readDoorOpenRaw();

  if (rawDoorState != lastRawDoorState) {
    lastDoorDebounceTime = millis();
    lastRawDoorState = rawDoorState;
  }

  if ((millis() - lastDoorDebounceTime) > DOOR_DEBOUNCE_DELAY) {
    if (rawDoorState != lastStableDoorState) {
      lastStableDoorState = rawDoorState;
      handleDoorChange(lastStableDoorState);
    }
  }
}

// ============================================================
//  OLED DRAW FUNCTIONS — 6 Screens
// ============================================================

// ---- Screen 0: Door icon + NTP clock only ----
void drawDoorIcon(bool open) {
  if (open) {
    // Body (cabinet): solid panel with a few shelf lines cut into it
    u8g2.drawBox(20, 2, 16, 24);
    u8g2.setDrawColor(0);
    u8g2.drawHLine(22, 9, 12);
    u8g2.drawHLine(22, 15, 12);
    u8g2.drawHLine(22, 21, 12);
    u8g2.setDrawColor(1);

    // Door: swung open, drawn as a tilted panel (2 triangles = 1 parallelogram)
    u8g2.drawTriangle(36, 4, 36, 24, 50, 21);
    u8g2.drawTriangle(36, 4, 50, 21, 50, 1);

    // Handle cut into the door
    u8g2.setDrawColor(0);
    u8g2.drawVLine(44, 9, 6);
    u8g2.setDrawColor(1);
  } else {
  // CLOSED: centered on screen, vertical handle cut into the left side
  u8g2.drawBox(28, 2, 16, 24);
  u8g2.setDrawColor(0);
  u8g2.drawVLine(32, 6, 7);
  u8g2.setDrawColor(1);
  }
}

void drawIconTimeScreen() {
  u8g2.clearBuffer();

  drawDoorIcon(doorIsOpen);

  u8g2.drawHLine(0, 30, 72);

  struct tm t;
  char timeBuf[9] = "--:--:--";
  if (getLocalTime(&t)) {
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  }

  u8g2.setFont(u8g2_font_5x7_tr);
  int strW = u8g2.getStrWidth(timeBuf);
  u8g2.drawStr((72 - strW) / 2, 38, timeBuf);

  u8g2.sendBuffer();
}

// ---- Screen 1: Live Fridge Status (unchanged) ----
void drawLiveStatus() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  u8g2.drawStr(0, 8, "FRIDGE STATUS");
  u8g2.drawHLine(0, 10, 72);

  u8g2.drawStr(0, 20, doorIsOpen ? "Door: OPEN" : "Door: CLOSED");

  char tempStr[16];
  snprintf(tempStr, sizeof(tempStr), "Temp: %.1fC", currentTemp);
  u8g2.drawStr(0, 30, tempStr);

  char wifiStr[20];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(wifiStr, sizeof(wifiStr), "WiFi: %s", WiFi.SSID().c_str());
  } else {
    snprintf(wifiStr, sizeof(wifiStr), "WiFi: !!");
  }
  u8g2.drawStr(0, 40, wifiStr);

  u8g2.sendBuffer();
}

// ---- Screen 2: Today's Stats (unchanged) ----
void drawStats() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  u8g2.drawStr(0, 8, "TODAY STATS");
  u8g2.drawHLine(0, 10, 72);

  char line[24];

  snprintf(line, sizeof(line), "Opens: %d", totalOpenings);
  u8g2.drawStr(0, 20, line);

  snprintf(line, sizeof(line), "Total: %lum %lus", totalOpenSeconds / 60, totalOpenSeconds % 60);
  u8g2.drawStr(0, 30, line);

  snprintf(line, sizeof(line), "Longest: %lus", longestOpening);
  u8g2.drawStr(0, 40, line);

  u8g2.sendBuffer();
}

// ---- Screen 3: Last Events (unchanged) ----
void drawEvents() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  u8g2.drawStr(0, 8, "LAST EVENTS");
  u8g2.drawHLine(0, 10, 72);

  for (int i = 0; i < 3; i++) {
    char line[24];
    snprintf(line, sizeof(line), "%s %lus", lastEvents[i].time, lastEvents[i].duration);
    u8g2.drawStr(0, 20 + i * 10, line);
  }

  u8g2.sendBuffer();
}

// ---- Screen 4: Alert History (unchanged) ----
void drawAlerts() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  u8g2.drawStr(0, 8, "ALERTS");
  u8g2.drawHLine(0, 10, 72);

  u8g2.drawStr(0, 22, lastAlertMsg.c_str());
  u8g2.drawStr(0, 34, lastAlertTime);

  u8g2.sendBuffer();
}

// ---- Screen 5: System Info (unchanged) ----
void drawSystem() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  u8g2.drawStr(0, 8, "SYSTEM INFO");
  u8g2.drawHLine(0, 10, 72);

  char ipStr[20];
  snprintf(ipStr, sizeof(ipStr), "%s", WiFi.localIP().toString().c_str());
  u8g2.drawStr(0, 20, ipStr);

  char rssiStr[20];
  snprintf(rssiStr, sizeof(rssiStr), "Signal:%d", WiFi.RSSI());
  u8g2.drawStr(0, 30, rssiStr);

  unsigned long upSec = millis() / 1000;
  char upStr[20];
  snprintf(upStr, sizeof(upStr), "Up:%luh%lum", upSec / 3600, (upSec % 3600) / 60);
  u8g2.drawStr(0, 40, upStr);

  u8g2.sendBuffer();
}

// ---- Maintenance Mode screen (only shown while active) ----
void drawMaintenance() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);

  u8g2.drawStr(0, 8, "MAINTENANCE");
  u8g2.drawHLine(0, 10, 72);

  long elapsedMs = (long)(millis() - maintenanceStartTime);
  long remainingMs = (long)maintenanceDurationMs - elapsedMs;
  if (remainingMs < 0) remainingMs = 0;
  unsigned long remainingSec = remainingMs / 1000;

  char line[24];
  snprintf(line, sizeof(line), "%02lum %02lus left", remainingSec / 60, remainingSec % 60);
  u8g2.drawStr(0, 22, line);

  u8g2.drawStr(0, 34, "Hold 3s to end");

  u8g2.sendBuffer();
}


// ============================================================
//  PERSISTENT DAILY STATS — ESP32 FLASH / NVS
// ============================================================
String todayDateString() {
  struct tm t;
  if (!getLocalTime(&t)) return "";

  char dateStr[12];
  snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return String(dateStr);
}

DailyStatsSnapshot makeCurrentStatsSnapshot(String dateValue) {
  DailyStatsSnapshot snap;
  snap.date = dateValue;
  snap.totalOpenings = totalOpenings;
  snap.totalOpenSeconds = totalOpenSeconds;
  snap.longestOpening = longestOpening;
  snap.tempSum = tempSum;
  snap.tempSampleCount = tempSampleCount;
  snap.tempMinToday = tempMinToday;
  snap.tempMaxToday = tempMaxToday;
  snap.totalTempDelta = totalTempDelta;
  snap.lastTempDelta = lastTempDelta;

  for (int i = 0; i < 3; i++) {
    snap.events[i] = lastEvents[i];
  }

  return snap;
}

void applyStatsSnapshotToRAM(DailyStatsSnapshot snap) {
  totalOpenings = snap.totalOpenings;
  totalOpenSeconds = snap.totalOpenSeconds;
  longestOpening = snap.longestOpening;
  tempSum = snap.tempSum;
  tempSampleCount = snap.tempSampleCount;
  tempMinToday = snap.tempMinToday;
  tempMaxToday = snap.tempMaxToday;
  totalTempDelta = snap.totalTempDelta;
  lastTempDelta = snap.lastTempDelta;

  for (int i = 0; i < 3; i++) {
    lastEvents[i] = snap.events[i];
  }
}

void resetDailyStatsInRAM() {
  totalOpenings = 0;
  totalOpenSeconds = 0;
  longestOpening = 0;
  totalTempDelta = 0.0;
  lastTempDelta = 0.0;
  tempSum = 0.0;
  tempSampleCount = 0;
  tempMinToday = 999.0;
  tempMaxToday = -999.0;

  for (int i = 0; i < 3; i++) {
    strncpy(lastEvents[i].time, "--:--", sizeof(lastEvents[i].time));
    lastEvents[i].duration = 0;
    lastEvents[i].tempDelta = 0.0;
  }
}

void saveCurrentStatsToFlash() {
  statsPrefs.begin("fg_stats", false);

  statsPrefs.putString("date", activeStatsDate);
  statsPrefs.putInt("opens", totalOpenings);
  statsPrefs.putULong("openSec", totalOpenSeconds);
  statsPrefs.putULong("longest", longestOpening);
  statsPrefs.putFloat("tempSum", tempSum);
  statsPrefs.putInt("tempCount", tempSampleCount);
  statsPrefs.putFloat("tempMin", tempMinToday);
  statsPrefs.putFloat("tempMax", tempMaxToday);
  statsPrefs.putFloat("totDelta", totalTempDelta);
  statsPrefs.putFloat("lastDelta", lastTempDelta);

  for (int i = 0; i < 3; i++) {
    String prefix = "e" + String(i);
    statsPrefs.putString((prefix + "time").c_str(), String(lastEvents[i].time));
    statsPrefs.putULong((prefix + "dur").c_str(), lastEvents[i].duration);
    statsPrefs.putFloat((prefix + "delta").c_str(), lastEvents[i].tempDelta);
  }

  // Save weekly rolling buffer
  statsPrefs.putInt("wkFilled", weeklyDaysFilled);
  for (int i = 0; i < WEEKLY_DAYS; i++) {
    statsPrefs.putFloat(("wkT" + String(i)).c_str(), weeklyAvgTemp[i]);
    statsPrefs.putInt(("wkO" + String(i)).c_str(),   weeklyOpenings[i]);
    statsPrefs.putFloat(("wkD" + String(i)).c_str(), weeklyAvgDelta[i]);
  }

  statsPrefs.end();
  Serial.println("Daily stats saved to ESP32 flash.");
}

void loadCurrentStatsFromFlash() {
  statsPrefs.begin("fg_stats", true);

  activeStatsDate = statsPrefs.getString("date", "");
  totalOpenings = statsPrefs.getInt("opens", 0);
  totalOpenSeconds = statsPrefs.getULong("openSec", 0);
  longestOpening = statsPrefs.getULong("longest", 0);
  tempSum = statsPrefs.getFloat("tempSum", 0.0);
  tempSampleCount = statsPrefs.getInt("tempCount", 0);
  tempMinToday = statsPrefs.getFloat("tempMin", 999.0);
  tempMaxToday = statsPrefs.getFloat("tempMax", -999.0);
  totalTempDelta = statsPrefs.getFloat("totDelta", 0.0);
  lastTempDelta = statsPrefs.getFloat("lastDelta", 0.0);

  for (int i = 0; i < 3; i++) {
    String prefix = "e" + String(i);
    String eventTime = statsPrefs.getString((prefix + "time").c_str(), "--:--");
    strncpy(lastEvents[i].time, eventTime.c_str(), sizeof(lastEvents[i].time) - 1);
    lastEvents[i].time[sizeof(lastEvents[i].time) - 1] = '\0';
    lastEvents[i].duration = statsPrefs.getULong((prefix + "dur").c_str(), 0);
    lastEvents[i].tempDelta = statsPrefs.getFloat((prefix + "delta").c_str(), 0.0);
  }

  // Restore weekly rolling buffer
  weeklyDaysFilled = statsPrefs.getInt("wkFilled", 0);
  for (int i = 0; i < WEEKLY_DAYS; i++) {
    weeklyAvgTemp[i]  = statsPrefs.getFloat(("wkT" + String(i)).c_str(), 0.0);
    weeklyOpenings[i] = statsPrefs.getInt(("wkO"   + String(i)).c_str(), 0);
    weeklyAvgDelta[i] = statsPrefs.getFloat(("wkD" + String(i)).c_str(), 0.0);
  }

  statsPrefs.end();

  Serial.print("Loaded saved daily stats from flash. Date: ");
  Serial.println(activeStatsDate.length() ? activeStatsDate : "none");
  Serial.print("Weekly buffer days filled: ");
  Serial.println(weeklyDaysFilled);
}

void savePendingSummaryToFlash(DailyStatsSnapshot snap) {
  statsPrefs.begin("fg_stats", false);

  statsPrefs.putBool("pend", true);
  statsPrefs.putString("pDate", snap.date);
  statsPrefs.putInt("pOpens", snap.totalOpenings);
  statsPrefs.putULong("pOpenSec", snap.totalOpenSeconds);
  statsPrefs.putULong("pLongest", snap.longestOpening);
  statsPrefs.putFloat("pTempSum", snap.tempSum);
  statsPrefs.putInt("pTempCnt", snap.tempSampleCount);
  statsPrefs.putFloat("pTempMin", snap.tempMinToday);
  statsPrefs.putFloat("pTempMax", snap.tempMaxToday);
  statsPrefs.putFloat("pTotDelta", snap.totalTempDelta);
  statsPrefs.putFloat("pLastDelta", snap.lastTempDelta);

  for (int i = 0; i < 3; i++) {
    String prefix = "pE" + String(i);
    statsPrefs.putString((prefix + "time").c_str(), String(snap.events[i].time));
    statsPrefs.putULong((prefix + "dur").c_str(), snap.events[i].duration);
    statsPrefs.putFloat((prefix + "delta").c_str(), snap.events[i].tempDelta);
  }

  statsPrefs.end();
  pendingSummaryWaiting = true;
  Serial.println("Yesterday snapshot saved as pending summary in flash.");
}

bool loadPendingSummaryFromFlash(DailyStatsSnapshot &snap) {
  statsPrefs.begin("fg_stats", true);

  bool pending = statsPrefs.getBool("pend", false);
  if (!pending) {
    statsPrefs.end();
    return false;
  }

  snap.date = statsPrefs.getString("pDate", "unknown-date");
  snap.totalOpenings = statsPrefs.getInt("pOpens", 0);
  snap.totalOpenSeconds = statsPrefs.getULong("pOpenSec", 0);
  snap.longestOpening = statsPrefs.getULong("pLongest", 0);
  snap.tempSum = statsPrefs.getFloat("pTempSum", 0.0);
  snap.tempSampleCount = statsPrefs.getInt("pTempCnt", 0);
  snap.tempMinToday = statsPrefs.getFloat("pTempMin", 999.0);
  snap.tempMaxToday = statsPrefs.getFloat("pTempMax", -999.0);
  snap.totalTempDelta = statsPrefs.getFloat("pTotDelta", 0.0);
  snap.lastTempDelta = statsPrefs.getFloat("pLastDelta", 0.0);

  for (int i = 0; i < 3; i++) {
    String prefix = "pE" + String(i);
    String eventTime = statsPrefs.getString((prefix + "time").c_str(), "--:--");
    strncpy(snap.events[i].time, eventTime.c_str(), sizeof(snap.events[i].time) - 1);
    snap.events[i].time[sizeof(snap.events[i].time) - 1] = '\0';
    snap.events[i].duration = statsPrefs.getULong((prefix + "dur").c_str(), 0);
    snap.events[i].tempDelta = statsPrefs.getFloat((prefix + "delta").c_str(), 0.0);
  }

  statsPrefs.end();
  pendingSummaryWaiting = true;
  return true;
}

void clearPendingSummaryFromFlash() {
  statsPrefs.begin("fg_stats", false);
  statsPrefs.putBool("pend", false);
  statsPrefs.end();
  pendingSummaryWaiting = false;
  Serial.println("Pending summary cleared from flash.");
}

String buildDailySummary(DailyStatsSnapshot snap) {
  float avgTemp = (snap.tempSampleCount > 0) ? (snap.tempSum / snap.tempSampleCount) : 0.0;
  float avgDelta = (snap.totalOpenings > 0) ? (snap.totalTempDelta / snap.totalOpenings) : 0.0;

  String summary = "DAILY SUMMARY - ";
  summary += snap.date;
  summary += "\n";
  summary += "Openings: " + String(snap.totalOpenings) + "\n";
  summary += "Total open time: " + String(snap.totalOpenSeconds / 60) + " min " + String(snap.totalOpenSeconds % 60) + " sec\n";
  summary += "Longest: " + String(snap.longestOpening) + " sec\n";
  summary += "Avg temp: " + String(avgTemp, 1) + "C\n";
  summary += "Min temp: " + String(snap.tempMinToday, 1) + "C\n";
  summary += "Max temp: " + String(snap.tempMaxToday, 1) + "C\n";
  summary += "Avg thermal impact/open: " + String(avgDelta, 1) + "C";

  return summary;
}

bool sendDailyStatsSnapshotToFirestore(DailyStatsSnapshot snap) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot write daily stats snapshot — WiFi not connected");
    return false;
  }

  float avgTemp = (snap.tempSampleCount > 0) ? (snap.tempSum / snap.tempSampleCount) : 0.0;
  float avgTempDelta = (snap.totalOpenings > 0) ? (snap.totalTempDelta / snap.totalOpenings) : 0.0;

  String statsURL = "https://firestore.googleapis.com/v1/projects/"
                    + String(FIREBASE_PROJECT_ID)
                    + "/databases/(default)/documents/daily_stats/"
                    + deviceId + "/days/" + snap.date
                    + "?key=" + String(FIREBASE_API_KEY);

  HTTPClient http;
  http.begin(statsURL);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"fields\": {";
  json += "\"date\": {\"stringValue\": \"" + snap.date + "\"},";
  json += "\"deviceId\": {\"stringValue\": \"" + deviceId + "\"},";
  json += "\"totalOpenings\": {\"integerValue\": " + String(snap.totalOpenings) + "},";
  json += "\"totalOpenSeconds\": {\"integerValue\": " + String(snap.totalOpenSeconds) + "},";
  json += "\"longestOpening\": {\"integerValue\": " + String(snap.longestOpening) + "},";
  json += "\"avgTemperature\": {\"doubleValue\": " + String(avgTemp, 1) + "},";
  json += "\"minTemperature\": {\"doubleValue\": " + String(snap.tempMinToday, 1) + "},";
  json += "\"maxTemperature\": {\"doubleValue\": " + String(snap.tempMaxToday, 1) + "},";
  json += "\"avgTempDelta\": {\"doubleValue\": " + String(avgTempDelta, 1) + "}";
  json += "}}";

  int httpCode = http.PATCH(json);
  http.end();

  if (httpCode == 200) {
    Serial.print("Daily stats snapshot written for ");
    Serial.println(snap.date);
    return true;
  }

  Serial.print("Daily stats snapshot write failed, code: ");
  Serial.println(httpCode);
  return false;
}

void trySendPendingDailySummary() {
  if (lastPendingSummaryAttempt != 0 && millis() - lastPendingSummaryAttempt < PENDING_SUMMARY_RETRY_MS) return;
  lastPendingSummaryAttempt = millis();

  DailyStatsSnapshot pending;
  if (!loadPendingSummaryFromFlash(pending)) return;

  String summary = buildDailySummary(pending);
  bool telegramOK = sendTelegram(summary);
  bool firebaseOK = sendDailyStatsSnapshotToFirestore(pending);

  if (telegramOK && firebaseOK) {
    clearPendingSummaryFromFlash();
    sendWeeklySummary();  // update 7-day averages now that a new day is confirmed
    lastAlertMsg = "Daily summary";
    updateLastAlertTime();
  } else {
    Serial.println("Pending summary NOT cleared — will retry later.");
  }
}

// ============================================================
//  FIREBASE / FIRESTORE — WEEKLY SUMMARY UPLOAD
//  Calculates averages from the 7-day rolling buffer and writes
//  a single document to weekly_summary/{deviceId} — overwritten
//  each day so the app always reads the latest 7-day picture.
// ============================================================
void sendWeeklySummary() {
  if (weeklyDaysFilled == 0 || WiFi.status() != WL_CONNECTED) return;

  float sumTemp = 0, sumDelta = 0;
  int   sumOpenings = 0;

  for (int i = 0; i < weeklyDaysFilled; i++) {
    sumTemp     += weeklyAvgTemp[i];
    sumOpenings += weeklyOpenings[i];
    sumDelta    += weeklyAvgDelta[i];
  }

  float weekAvgTemp     = sumTemp     / weeklyDaysFilled;
  float weekAvgDelta    = sumDelta    / weeklyDaysFilled;
  float weekAvgOpenings = (float)sumOpenings / weeklyDaysFilled;

  String url = "https://firestore.googleapis.com/v1/projects/"
               + String(FIREBASE_PROJECT_ID)
               + "/databases/(default)/documents/weekly_summary/"
               + deviceId
               + "?key=" + String(FIREBASE_API_KEY);

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"fields\": {";
  json += "\"weekAvgTemperature\": {\"doubleValue\": "   + String(weekAvgTemp, 1)     + "},";
  json += "\"weekAvgOpeningsPerDay\": {\"doubleValue\": " + String(weekAvgOpenings, 1) + "},";
  json += "\"weekAvgTempDelta\": {\"doubleValue\": "     + String(weekAvgDelta, 1)    + "},";
  json += "\"daysRecorded\": {\"integerValue\": "        + String(weeklyDaysFilled)   + "},";
  json += "\"deviceId\": {\"stringValue\": \""           + deviceId                   + "\"}";
  json += "}}";

  int httpCode = http.PATCH(json);
  http.end();

  if (httpCode == 200) {
    Serial.print("Weekly summary uploaded (");
    Serial.print(weeklyDaysFilled);
    Serial.println(" days)");
  } else {
    Serial.print("Weekly summary failed, code: ");
    Serial.println(httpCode);
  }
}

void checkDailyDateRollover() {
  String today = todayDateString();
  if (today.length() == 0) return;  // NTP not ready yet

  if (activeStatsDate.length() == 0) {
    activeStatsDate = today;
    saveCurrentStatsToFlash();
    return;
  }

  if (today != activeStatsDate) {
    // Freeze yesterday safely into a pending flash snapshot FIRST.
    // Then reset today's RAM/flash immediately so new events belong to the new day.
    DailyStatsSnapshot yesterday = makeCurrentStatsSnapshot(activeStatsDate);
    savePendingSummaryToFlash(yesterday);

    // Shift the weekly rolling buffer — drop oldest, insert yesterday at front
    float avgTemp  = (yesterday.tempSampleCount > 0)
                     ? (yesterday.tempSum / yesterday.tempSampleCount) : 0.0;
    float avgDelta = (yesterday.totalOpenings > 0)
                     ? (yesterday.totalTempDelta / yesterday.totalOpenings) : 0.0;

    for (int i = WEEKLY_DAYS - 1; i > 0; i--) {
      weeklyAvgTemp[i]  = weeklyAvgTemp[i - 1];
      weeklyOpenings[i] = weeklyOpenings[i - 1];
      weeklyAvgDelta[i] = weeklyAvgDelta[i - 1];
    }
    weeklyAvgTemp[0]  = avgTemp;
    weeklyOpenings[0] = yesterday.totalOpenings;
    weeklyAvgDelta[0] = avgDelta;
    if (weeklyDaysFilled < WEEKLY_DAYS) weeklyDaysFilled++;

    resetDailyStatsInRAM();
    activeStatsDate = today;
    saveCurrentStatsToFlash();  // also saves updated weekly buffer

    Serial.print("New day started. Stats reset for ");
    Serial.println(activeStatsDate);
    Serial.print("Weekly buffer now has ");
    Serial.print(weeklyDaysFilled);
    Serial.println(" day(s)");
  }

  if (WiFi.status() == WL_CONNECTED) {
    trySendPendingDailySummary();
  }
}

// ============================================================
//  ALERT LOGIC
// ============================================================
void checkAlerts() {
  unsigned long now = millis();

  // Auto-exit maintenance mode when its timer runs out.
  checkMaintenanceExpiry();

  if (!maintenanceMode) {
    // ---- Door alert: 60s then every 5 minutes while open ----
    if (doorIsOpen) {
      unsigned long openDuration = (millis() - doorOpenTime) / 1000;

      if (openDuration > DOOR_ALERT_SECONDS) {
        if (doorAlertCount == 0 || now - lastDoorAlertTime >= DOOR_ALERT_REPEAT_MS) {
          doorAlertCount++;
          lastDoorAlertTime = now;

          String msg = "WARNING: Fridge door is still open! Open for ";
          msg += String(openDuration);
          msg += " seconds.";

          sendTelegram(msg);
          logAlertToFirestore("door", msg);

          lastAlertMsg = "Door still open";
          updateLastAlertTime();
        }
      }
    } else {
      resetDoorAlertState();
    }

    // ---- Temperature alert: first on threshold, then every 10 minutes ----
    if (currentTemp > TEMP_THRESHOLD_C) {
      if (!tempAlertSent || now - lastTempAlertTime >= TEMP_ALERT_REPEAT_MS) {
        String msg = "TEMP ALERT: Temperature is ";
        msg += String(currentTemp, 1);
        msg += "C, above ";
        msg += String(TEMP_THRESHOLD_C, 1);
        msg += "C!";

        sendTelegram(msg);
        logAlertToFirestore("temperature", msg);

        lastAlertMsg = "Temp above threshold";
        updateLastAlertTime();

        tempAlertSent = true;
        lastTempAlertTime = now;
      }
    } else if (currentTemp < TEMP_THRESHOLD_C - TEMP_HYSTERESIS_C) {
      tempAlertSent = false;
      lastTempAlertTime = 0;
    }
  }

  // ---- Daily stats date rollover + safe summary sending ----
  // At the first loop after the date changes, yesterday is copied into
  // a pending flash snapshot, today's counters are reset, and the summary
  // is retried until Telegram + Firestore both succeed.
  checkDailyDateRollover();
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  // Generate a unique ID from this chip's built-in factory MAC address.
  // Every ESP32 has a different one, so every board running this exact
  // same code automatically gets its own private Firestore document.
  uint64_t chipId = ESP.getEfuseMac();
  char idBuf[13];
  snprintf(idBuf, sizeof(idBuf), "%04X%08X", (uint16_t)(chipId >> 32), (uint32_t)chipId);
  deviceId = String(idBuf);

  firestoreURL = "https://firestore.googleapis.com/v1/projects/"
                 + String(FIREBASE_PROJECT_ID)
                 + "/databases/(default)/documents/fridge_data/" + deviceId
                 + "?key=" + String(FIREBASE_API_KEY);

  // Same document, but fetched via GET to read the maintenanceCommand field
  // the app writes. We reuse the same URL — GET reads, PATCH writes.
  // Stored separately just for clarity in checkFirestoreCommand().

  Serial.print("This device's unique ID: ");
  Serial.println(deviceId);

  Serial.println();
  Serial.println("=== FridgeGuard IoT — Maintenance Mode Version — ONLINE ===");

  pinMode(LED_PIN, OUTPUT);

  // Temperature sensor
  sensors.begin();
  Serial.println("DS18B20 ready");

  // OLED
  u8g2.begin();
  u8g2.setContrast(255);
  Serial.println("OLED ready");

  // Door sensor — polling debounce, no interrupt
  pinMode(DOOR_PIN, INPUT_PULLUP);

  doorIsOpen = readDoorOpenRaw();
  lastStableDoorState = doorIsOpen;
  lastRawDoorState = doorIsOpen;

  if (doorIsOpen) {
    doorOpenTime = millis();
  }

  Serial.print("Initial door state: ");
  Serial.println(doorIsOpen ? "OPEN" : "CLOSED");
  Serial.println("Door monitor ready");

  // Button + interrupt — CHANGE so we can measure press duration
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, CHANGE);
  Serial.println("Button ready (short press = next screen, hold 3s = maintenance mode)");

  // WiFi
  connectWiFi();

  // NTP
  syncNTP();

  // Restore today's saved stats from ESP32 flash after reset/power loss.
  // If the saved date is yesterday, checkDailyDateRollover() will freeze it
  // as a pending summary, reset today, and send it when WiFi is available.
  loadCurrentStatsFromFlash();
  checkDailyDateRollover();

  // Clear old Telegram messages/buttons so old replies do not affect this run.
  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    clearOldTelegramUpdates();
    sendTelegramOnlineStatus();
  } else {
    wifiWasConnected = false;
    wifiLostAt = millis();
  }

  Serial.println("Setup complete — entering main loop.");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  // 1. Door detection
  updateDoor();

  // 2. WiFi status + auto-reconnect every 30 seconds if dropped
  if (WiFi.status() != WL_CONNECTED && wifiWasConnected) {
    wifiWasConnected = false;
    wifiLostAt = now;
    Serial.println("WiFi/Telegram connection lost. Cannot notify until connection returns.");
  }

  if (WiFi.status() != WL_CONNECTED && now - lastWifiCheck > WIFI_RECONNECT_INTERVAL_MS) {
    Serial.println("WiFi dropped — attempting reconnect...");
    connectWiFi();

    if (WiFi.status() == WL_CONNECTED) {
      if (!ntpSynced) {
        syncNTP();
      }

      unsigned long offlineMs = 0;

      if (wifiLostAt > 0) {
        offlineMs = now - wifiLostAt;
      }

      wifiWasConnected = true;
      wifiLostAt = 0;

      sendTelegramConnectionRestored(offlineMs);
    }

    lastWifiCheck = now;
  }

  // 3. Read temperature every 2 seconds
  if (now - lastTempRead > TEMP_READ_INTERVAL_MS) {
    sensors.requestTemperatures();

    float t = sensors.getTempCByIndex(0);

    if (t != DEVICE_DISCONNECTED_C) {
      currentTemp = t;

      // Accumulate for daily average, min, and max
      tempSum += currentTemp;
      tempSampleCount++;
      if (currentTemp < tempMinToday) tempMinToday = currentTemp;
      if (currentTemp > tempMaxToday) tempMaxToday = currentTemp;

      Serial.print("Temperature: ");
      Serial.print(currentTemp, 1);
      Serial.println(" C");

      if (now - lastStatsAutosave > STATS_AUTOSAVE_INTERVAL_MS) {
        saveCurrentStatsToFlash();
        lastStatsAutosave = now;
      }
    } else {
      Serial.println("ERROR: DS18B20 not found. Check wiring/resistor.");
    }

    lastTempRead = now;
  }

  // 4. Read Telegram replies/buttons every 3 seconds
  if (WiFi.status() == WL_CONNECTED && now - lastTelegramPoll > TELEGRAM_POLL_INTERVAL_MS) {
    processTelegramUpdates();
    lastTelegramPoll = now;
  }

  // 5. Check alerts (also handles maintenance auto-expiry)
  checkAlerts();

  // 6. Button presses
  if (longPressDetected) {
    longPressDetected = false;

    if (!maintenanceMode) {
      enterMaintenanceMode();
    } else {
      requestMaintenanceExitConfirmation();
    }
  }

  if (buttonPressed) {
    buttonPressed = false;

    // Screen cycling is disabled while in maintenance mode — the
    // maintenance countdown screen takes over the whole display.
    if (!maintenanceMode) {
      currentScreen = (currentScreen + 1) % SCREEN_COUNT;
    }
  }

  // 7. Refresh OLED
  if (now - lastScreenRefresh > SCREEN_REFRESH_INTERVAL_MS) {
    if (maintenanceMode) {
      drawMaintenance();
    } else {
      switch (currentScreen) {
        case SCREEN_ICON_TIME:
          drawIconTimeScreen();
          break;

        case SCREEN_LIVE_STATUS:
          drawLiveStatus();
          break;

        case SCREEN_STATS:
          drawStats();
          break;

        case SCREEN_EVENTS:
          drawEvents();
          break;

        case SCREEN_ALERTS:
          drawAlerts();
          break;

        case SCREEN_SYSTEM:
          drawSystem();
          break;
      }
    }

    lastScreenRefresh = now;
  }

  // 8. Heartbeat LED blink
  if (now - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    lastHeartbeat = now;
  }

  // 9. Upload current status to Firestore every 12 seconds
  if (now - lastCloudUpload > CLOUD_UPLOAD_INTERVAL_MS) {
    sendToFirestore();
    lastCloudUpload = now;
  }

  // 10. Check for app commands (maintenance start/end) every 5 seconds
  // This is a Firestore GET read — counts against the 50,000 reads/day
  // free limit, not the 20,000 writes limit. At 5s intervals per board:
  // 17,280 reads/day × 2 boards = 34,560 — within the free tier.
  if (now - lastCommandCheck > COMMAND_CHECK_INTERVAL_MS) {
    checkFirestoreCommand();
    lastCommandCheck = now;
  }
}
