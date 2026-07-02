// ============================================================
//  FridgeGuard IoT — Full Sketch WITHOUT the Remote App (No Firebase)
//  
//  FINAL VERSION (pre-Firebase) — includes:
//    - WiFi + auto-reconnect, NTP, OLED 6-screen dashboard
//    - DS18B20 temperature + reed switch door detection
//    - Telegram alerts with REPEATING reminders (door + temperature)
//    - Maintenance Mode: long-press the button to pause alerts during
//      cleaning/repair, with a hard 20-minute auto-expiry as a fail-safe
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>

#include "secrets.h"
// ^ Copy secrets_template.h -> secrets.h in this folder and fill in
//   your own WiFi and Telegram Bot values before uploading. See README.md.

// ---------- Pin Definitions ----------
#define LED_PIN    8   // Onboard LED (heartbeat)
#define TEMP_PIN   4   // DS18B20 data wire
#define DOOR_PIN   3   // Reed switch
#define BUTTON_PIN 9   // BOOT button (tap = next screen, hold 3s = maintenance toggle)

// ---------- Temperature Sensor ----------
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);
float currentTemp = 0.0;

// ---------- OLED Display (0.42" 72x40 SSD1306) ----------
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);

// ---------- Door / Reed Switch State (set inside ISR) ----------
volatile bool doorJustChanged = false;
volatile bool doorIsOpen = false;
volatile unsigned long lastInterruptTime = 0;
unsigned long doorOpenTime = 0;

// ---------- Button / Screen State Machine ----------
#define SCREEN_COUNT 6
int currentScreen = 0;
volatile bool buttonPressed = false;             // short tap -> next screen
volatile bool buttonLongPressTriggered = false;  // long hold -> toggle Maintenance Mode
volatile bool buttonCurrentlyDown = false;
volatile unsigned long buttonDownTime = 0;
volatile unsigned long lastButtonInterrupt = 0;
#define LONG_PRESS_MS 3000UL   // hold 3 seconds to toggle Maintenance Mode

// ---------- Maintenance Mode ----------
bool maintenanceMode = false;
unsigned long maintenanceStartTime = 0;
#define MAINTENANCE_DURATION_MS 1200000UL   // hard auto-expiry after 20 minutes

// ---------- Daily Statistics ----------
int totalOpenings = 0;
unsigned long totalOpenSeconds = 0;
unsigned long longestOpening = 0;

struct DoorEvent {
  char time[12];
  unsigned long duration;
};
DoorEvent lastEvents[3] = { {"--:--", 0}, {"--:--", 0}, {"--:--", 0} };

String lastAlertMsg = "None yet";
char lastAlertTime[12] = "--:--";

// ---------- Alert Thresholds & Tracking ----------
#define DOOR_ALERT_SECONDS    60
#define DOOR_ALERT_REPEAT_MS  300000UL    // repeat every 5 min while still open
#define TEMP_THRESHOLD_C      8.0
#define TEMP_ALERT_REPEAT_MS  1200000UL   // repeat every 20 min while still high
#define SUMMARY_HOUR          0

bool doorAlertSent = false;
unsigned long lastDoorAlertTime = 0;

bool tempAlertSent = false;
unsigned long lastTempAlertTime = 0;

bool summarySentToday = false;

// ---------- Non-Blocking Timers ----------
unsigned long lastTempRead = 0;
#define TEMP_READ_INTERVAL_MS 2000UL

unsigned long lastWifiCheck = 0;
#define WIFI_RECONNECT_INTERVAL_MS 30000UL

unsigned long lastScreenRefresh = 0;
#define SCREEN_REFRESH_INTERVAL_MS 200UL

unsigned long lastHeartbeat = 0;
#define HEARTBEAT_INTERVAL_MS 1000UL
bool ledState = false;

// ============================================================
//  INTERRUPT SERVICE ROUTINES
// ============================================================
void IRAM_ATTR doorISR() {
  unsigned long now = millis();
  if (now - lastInterruptTime < 50) return;       // debounce: ignore <50ms
  lastInterruptTime = now;
  doorIsOpen = (digitalRead(DOOR_PIN) == HIGH);   // HIGH = open
  doorJustChanged = true;
}

// Detects both a short tap (advance screen) and a long hold (toggle Maintenance Mode).
// Triggers on CHANGE so it sees both the press-down and release edges.
void IRAM_ATTR buttonISR() {
  unsigned long now = millis();
  if (now - lastButtonInterrupt < 50) return;     // debounce
  lastButtonInterrupt = now;

  bool isDown = (digitalRead(BUTTON_PIN) == LOW); // LOW = pressed (INPUT_PULLUP)

  if (isDown) {
    buttonDownTime = now;
    buttonCurrentlyDown = true;
  } else if (buttonCurrentlyDown) {
    unsigned long heldFor = now - buttonDownTime;
    if (heldFor >= LONG_PRESS_MS) {
      buttonLongPressTriggered = true;
    } else {
      buttonPressed = true;
    }
    buttonCurrentlyDown = false;
  }
}

// ============================================================
//  WIFI
// ============================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connect attempt timed out — will retry in loop().");
  }
}

// ============================================================
//  TELEGRAM
// ============================================================
String urlEncode(String str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}

void sendTelegram(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot send Telegram — WiFi not connected");
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/sendMessage?chat_id=";
  url += CHAT_ID;
  url += "&text=";
  url += urlEncode(message);
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    Serial.println("Telegram sent successfully");
  } else {
    Serial.print("Telegram failed, code: ");
    Serial.println(httpCode);
  }
  http.end();
}

// ============================================================
//  OLED DRAW FUNCTIONS — 6 Screens + Maintenance Overlay
// ============================================================

// Screen 0 — Door status icon + live clock underneath
void drawDoorIcon() {
  u8g2.clearBuffer();

  int x = 22, y = 0, w = 28, h = 20;   // icon bounding box, centered horizontally

  if (doorIsOpen) {
    // OPEN: classic architectural "swinging door" symbol — a quarter-circle
    // swing path plus the door leaf at its open angle, hinged top-right
    int hingeX = x + w;
    int hingeY = y;
    int radius = h;
    u8g2.drawCircle(hingeX, hingeY, radius, U8G2_DRAW_LOWER_LEFT);
    int leafX = hingeX - (int)(radius * 0.71);   // ~45 degrees open
    int leafY = hingeY + (int)(radius * 0.71);
    u8g2.drawLine(hingeX, hingeY, leafX, leafY);
  } else {
    // CLOSED: solid filled door, with a small punched-out dot for the handle
    u8g2.drawBox(x, y, w, h);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(x + w - 5, y + h / 2, 2);
    u8g2.setDrawColor(1);
  }

  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(doorIsOpen ? 24 : 18, 28, doorIsOpen ? "OPEN" : "CLOSED");

  // Live NTP clock, right under the door status
  struct tm t;
  char timeStr[12] = "--:--:--";
  if (getLocalTime(&t)) {
    sprintf(timeStr, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  }
  u8g2.drawStr(12, 38, timeStr);

  u8g2.sendBuffer();
}

// Screen 1 — Fridge Status (reverted to original Door/Temp/WiFi layout)
void drawLiveStatus() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 8, "FRIDGE STATUS");
  u8g2.drawHLine(0, 10, 72);
  u8g2.drawStr(0, 20, doorIsOpen ? "Door: OPEN" : "Door: CLOSED");
  char tempStr[16];
  sprintf(tempStr, "Temp: %.1fC", currentTemp);
  u8g2.drawStr(0, 30, tempStr);
  u8g2.drawStr(0, 40, WiFi.status() == WL_CONNECTED ? "WiFi: ON" : "WiFi: !!");
  u8g2.sendBuffer();
}

void drawStats() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 8, "TODAY STATS");
  u8g2.drawHLine(0, 10, 72);
  char line[24];
  sprintf(line, "Opens: %d", totalOpenings);
  u8g2.drawStr(0, 20, line);
  sprintf(line, "Total: %lum %lus", totalOpenSeconds / 60, totalOpenSeconds % 60);
  u8g2.drawStr(0, 30, line);
  sprintf(line, "Longest: %lus", longestOpening);
  u8g2.drawStr(0, 40, line);
  u8g2.sendBuffer();
}

void drawEvents() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 8, "LAST EVENTS");
  u8g2.drawHLine(0, 10, 72);
  for (int i = 0; i < 3; i++) {
    char line[24];
    sprintf(line, "%s %lus", lastEvents[i].time, lastEvents[i].duration);
    u8g2.drawStr(0, 20 + i * 10, line);
  }
  u8g2.sendBuffer();
}

void drawAlerts() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 8, "ALERTS");
  u8g2.drawHLine(0, 10, 72);
  u8g2.drawStr(0, 22, lastAlertMsg.c_str());
  u8g2.drawStr(0, 34, lastAlertTime);
  u8g2.sendBuffer();
}

// Screen 5 — System Info, now including the WiFi network name
void drawSystem() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 8, "SYSTEM INFO");
  u8g2.drawHLine(0, 10, 72);

  char ssidStr[33];   // WiFi SSIDs can be up to 32 chars; long names will visually clip on screen
  sprintf(ssidStr, "%s", WIFI_SSID);
  u8g2.drawStr(0, 18, ssidStr);

  char ipStr[20];
  sprintf(ipStr, "%s", WiFi.localIP().toString().c_str());
  u8g2.drawStr(0, 25, ipStr);

  char rssiStr[20];
  sprintf(rssiStr, "Sig: %d dBm", WiFi.RSSI());
  u8g2.drawStr(0, 32, rssiStr);

  unsigned long upSec = millis() / 1000;
  char upStr[20];
  sprintf(upStr, "Up: %luh %lum", upSec / 3600, (upSec % 3600) / 60);
  u8g2.drawStr(0, 39, upStr);

  u8g2.sendBuffer();
}

// Maintenance Mode overlay — takes over the whole display while active,
// so it's unmistakable that alerts are paused, with a live countdown.
void drawMaintenance() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 9, "MAINTENANCE");
  u8g2.drawStr(10, 18, "MODE ACTIVE");
  u8g2.drawHLine(0, 20, 72);

  unsigned long elapsed = millis() - maintenanceStartTime;
  unsigned long remainingMin = (elapsed < MAINTENANCE_DURATION_MS)
                                ? (MAINTENANCE_DURATION_MS - elapsed) / 60000
                                : 0;
  char line[20];
  sprintf(line, "Ends in: %lum", remainingMin);
  u8g2.drawStr(2, 30, line);
  u8g2.drawStr(2, 39, "Alerts paused");

  u8g2.sendBuffer();
}

// ============================================================
//  ALERT LOGIC — door / temperature (both now repeat) / daily summary
// ============================================================
void checkAlerts() {
  // Auto-expire Maintenance Mode after its hard time limit
  if (maintenanceMode && millis() - maintenanceStartTime > MAINTENANCE_DURATION_MS) {
    maintenanceMode = false;
    sendTelegram("Maintenance mode expired - alerts resumed automatically.");
    Serial.println("Maintenance mode auto-expired");
  }

  // ---------- Door alert: first at 60s, then repeats every 5 min ----------
  if (doorIsOpen) {
    unsigned long openDuration = (millis() - doorOpenTime) / 1000;
    if (openDuration > DOOR_ALERT_SECONDS && !maintenanceMode) {
      if (!doorAlertSent) {
        sendTelegram("WARNING: Fridge door has been open for over 60 seconds!");
        lastAlertMsg = "Door open 60s+";
        doorAlertSent = true;
        lastDoorAlertTime = millis();
      } else if (millis() - lastDoorAlertTime > DOOR_ALERT_REPEAT_MS) {
        String msg = "Door STILL open - " + String(openDuration / 60) + "m " +
                     String(openDuration % 60) + "s now!";
        sendTelegram(msg);
        lastAlertMsg = "Door still open";
        lastDoorAlertTime = millis();
      }
    }
  } else {
    doorAlertSent = false;   // reset once door closes
  }

  // ---------- Temperature alert: first at threshold, then repeats every 20 min ----------
  if (currentTemp > TEMP_THRESHOLD_C && !maintenanceMode) {
    if (!tempAlertSent) {
      String msg = "TEMP ALERT: Fridge is " + String(currentTemp, 1) + "C!";
      sendTelegram(msg);
      lastAlertMsg = msg;
      tempAlertSent = true;
      lastTempAlertTime = millis();
    } else if (millis() - lastTempAlertTime > TEMP_ALERT_REPEAT_MS) {
      String msg = "Temp STILL high: " + String(currentTemp, 1) + "C";
      sendTelegram(msg);
      lastAlertMsg = "Temp still high";
      lastTempAlertTime = millis();
    }
  }
  if (currentTemp < TEMP_THRESHOLD_C - 1.0) {
    tempAlertSent = false;   // 1C hysteresis prevents flicker-spam at the boundary
  }

  // ---------- Daily summary at midnight ----------
  struct tm t;
  if (getLocalTime(&t)) {
    sprintf(lastAlertTime, "%02d:%02d", t.tm_hour, t.tm_min);
    if (t.tm_hour == SUMMARY_HOUR && t.tm_min == 0 && !summarySentToday) {
      String summary = "DAILY SUMMARY\n";
      summary += "Openings: " + String(totalOpenings) + "\n";
      summary += "Total open time: " + String(totalOpenSeconds / 60) + " min\n";
      summary += "Longest: " + String(longestOpening) + " sec";
      sendTelegram(summary);
      summarySentToday = true;
      totalOpenings = 0;
      totalOpenSeconds = 0;
      longestOpening = 0;
    }
    if (t.tm_hour != SUMMARY_HOUR) summarySentToday = false;
  }
}

// ============================================================
//  SETUP — runs once
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== FridgeGuard IoT (No App/No Firebase) — ONLINE ===");

  pinMode(LED_PIN, OUTPUT);

  sensors.begin();
  Serial.println("DS18B20 ready");

  u8g2.begin();
  u8g2.setContrast(255);
  Serial.println("OLED ready");

  pinMode(DOOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(DOOR_PIN), doorISR, CHANGE);
  doorIsOpen = (digitalRead(DOOR_PIN) == HIGH);
  Serial.println("Door monitor ready");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, CHANGE);  // CHANGE so we can detect long-press
  Serial.println("Button ready");

  connectWiFi();

  configTime(3 * 3600, 0, "pool.ntp.org");
  Serial.print("Waiting for NTP time sync");
  struct tm timeInfo;
  unsigned long ntpStart = millis();
  while (!getLocalTime(&timeInfo) && millis() - ntpStart < 10000) {
    Serial.print(".");
    delay(500);
  }
  if (getLocalTime(&timeInfo)) {
    Serial.println("\nNTP synced!");
  } else {
    Serial.println("\nNTP sync FAILED — check WiFi/internet connection.");
  }

  Serial.println("Setup complete — entering main loop.");
}

// ============================================================
//  LOOP — runs forever, nothing here blocks anything else
// ============================================================
void loop() {
  unsigned long now = millis();

  // 1. Auto-reconnect WiFi every 30 seconds if dropped
  if (WiFi.status() != WL_CONNECTED && now - lastWifiCheck > WIFI_RECONNECT_INTERVAL_MS) {
    Serial.println("WiFi dropped — attempting reconnect...");
    connectWiFi();
    lastWifiCheck = now;
  }

  // 2. Read temperature every 2 seconds
  if (now - lastTempRead > TEMP_READ_INTERVAL_MS) {
    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C) {
      currentTemp = t;
    } else {
      Serial.println("ERROR: DS18B20 not found. Check wiring/resistor.");
    }
    lastTempRead = now;
  }

  // 3. Handle door open/close events
  if (doorJustChanged) {
    doorJustChanged = false;
    struct tm t;
    char timeStr[12] = "--:--";
    if (getLocalTime(&t)) sprintf(timeStr, "%02d:%02d", t.tm_hour, t.tm_min);

    if (doorIsOpen) {
      doorOpenTime = millis();
      Serial.println("DOOR OPENED at " + String(timeStr));
    } else {
      unsigned long duration = (millis() - doorOpenTime) / 1000;
      totalOpenings++;
      totalOpenSeconds += duration;
      if (duration > longestOpening) longestOpening = duration;

      lastEvents[2] = lastEvents[1];
      lastEvents[1] = lastEvents[0];
      strncpy(lastEvents[0].time, timeStr, sizeof(lastEvents[0].time) - 1);
      lastEvents[0].time[sizeof(lastEvents[0].time) - 1] = '\0';
      lastEvents[0].duration = duration;

      Serial.println("DOOR CLOSED at " + String(timeStr) + ", open for " + String(duration) + "s");

      // Closing the door also ends Maintenance Mode early, if it was active
      if (maintenanceMode) {
        maintenanceMode = false;
        sendTelegram("Maintenance mode ended - door closed, alerts resumed.");
        Serial.println("Maintenance mode ended (door closed)");
      }
    }
  }

  // 4. Check door / temperature / daily-summary alerts (+ Maintenance Mode expiry)
  checkAlerts();

  // 5. Button: long-press toggles Maintenance Mode, short tap advances screen
  if (buttonLongPressTriggered) {
    buttonLongPressTriggered = false;
    maintenanceMode = !maintenanceMode;
    if (maintenanceMode) {
      maintenanceStartTime = millis();
      sendTelegram("Maintenance mode ON - alerts paused for 20 minutes.");
      Serial.println("Maintenance mode ON");
    } else {
      sendTelegram("Maintenance mode OFF - alerts resumed.");
      Serial.println("Maintenance mode OFF");
    }
  }

  if (buttonPressed) {
    buttonPressed = false;
    currentScreen = (currentScreen + 1) % SCREEN_COUNT;
  }

  // 6. Refresh OLED ~5 times per second
  if (now - lastScreenRefresh > SCREEN_REFRESH_INTERVAL_MS) {
    if (maintenanceMode) {
      drawMaintenance();
    } else {
      switch (currentScreen) {
        case 0: drawDoorIcon();   break;
        case 1: drawLiveStatus(); break;
        case 2: drawStats();      break;
        case 3: drawEvents();     break;
        case 4: drawAlerts();     break;
        case 5: drawSystem();     break;
      }
    }
    lastScreenRefresh = now;
  }

  // 7. Heartbeat LED blink (1 second on/off) — proves the loop is alive
  if (now - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);  // swap HIGH/LOW if your LED is inverted
    lastHeartbeat = now;
  }
}
