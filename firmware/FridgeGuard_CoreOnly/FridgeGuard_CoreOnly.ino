// ============================================================
//  FridgeGuard IoT — Core Systems Test (No Telegram / No Firestore)
//
//  Verifies, all in one sketch:
//    - WiFi connection + auto-reconnect
//    - NTP time sync
//    - OLED 4-screen dashboard + button cycling
//    - DS18B20 temperature readings
//    - Reed switch door open/close detection + stats
//
//  Deliberately leaves OUT Telegram and Firebase so you can confirm
//  every sensor / display / network piece works before adding those back.
// ============================================================

#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>

#include "secrets.h"
// ^ Copy secrets_template.h -> secrets.h in this folder and fill in
//   your own WiFi credentials before uploading. See README.md.

// ---------- Pin Definitions ----------
#define LED_PIN    8   // Onboard LED (heartbeat)
#define TEMP_PIN   4   // DS18B20 data wire
#define DOOR_PIN   3   // Reed switch
#define BUTTON_PIN 9   // BOOT button (screen cycling)

// ---------- Temperature Sensor ----------
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);
float currentTemp = 0.0;

// ---------- OLED Display (0.42" 72x40 SSD1306) ----------
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);

// ---------- Door / Reed Switch State ----------
volatile bool doorJustChanged = false;
volatile bool doorIsOpen = false;
volatile unsigned long lastInterruptTime = 0;
unsigned long doorOpenTime = 0;

// ---------- Button / Screen State Machine ----------
#define SCREEN_COUNT 4
int currentScreen = 0;
volatile bool buttonPressed = false;
volatile unsigned long lastButtonTime = 0;

// ---------- Daily Statistics ----------
int totalOpenings = 0;
unsigned long totalOpenSeconds = 0;
unsigned long longestOpening = 0;

struct DoorEvent {
  char time[12];
  unsigned long duration;
};
DoorEvent lastEvents[3] = { {"--:--", 0}, {"--:--", 0}, {"--:--", 0} };

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

void IRAM_ATTR buttonISR() {
  unsigned long now = millis();
  if (now - lastButtonTime > 200) {               // debounce: 200ms
    buttonPressed = true;
    lastButtonTime = now;
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
    Serial.print("Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("\nWiFi connect attempt timed out — will retry in loop().");
  }
}

// ============================================================
//  OLED DRAW FUNCTIONS — 4 Screens
// ============================================================
void drawLiveStatus() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 8, "LIVE STATUS");
  u8g2.drawHLine(0, 10, 72);

  u8g2.drawStr(0, 20, doorIsOpen ? "Door: OPEN" : "Door: CLOSED");

  char tempStr[16];
  sprintf(tempStr, "Temp: %.1fC", currentTemp);
  u8g2.drawStr(0, 30, tempStr);

  // Live NTP-synced clock — doubles as a constant NTP check
  struct tm t;
  char timeStr[16] = "Time: --:--:--";
  if (getLocalTime(&t)) {
    sprintf(timeStr, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  }
  u8g2.drawStr(0, 40, timeStr);

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

void drawSystem() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 8, "SYSTEM INFO");
  u8g2.drawHLine(0, 10, 72);
  char ipStr[20];
  sprintf(ipStr, "%s", WiFi.localIP().toString().c_str());
  u8g2.drawStr(0, 20, ipStr);
  char rssiStr[20];
  sprintf(rssiStr, "Sig: %d dBm", WiFi.RSSI());
  u8g2.drawStr(0, 30, rssiStr);
  unsigned long upSec = millis() / 1000;
  char upStr[20];
  sprintf(upStr, "Up: %luh %lum", upSec / 3600, (upSec % 3600) / 60);
  u8g2.drawStr(0, 40, upStr);
  u8g2.sendBuffer();
}

// ============================================================
//  SETUP — runs once
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== FridgeGuard Core Test — ONLINE ===");

  pinMode(LED_PIN, OUTPUT);

  sensors.begin();
  Serial.println("DS18B20 ready");

  u8g2.begin();
  u8g2.setContrast(255);
  Serial.println("OLED ready");

  pinMode(DOOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(DOOR_PIN), doorISR, CHANGE);
  doorIsOpen = (digitalRead(DOOR_PIN) == HIGH);   // read initial state at boot
  Serial.println("Door monitor ready");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
  Serial.println("Button ready");

  connectWiFi();

  // NTP sync — Lebanon is UTC+3 in summer
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
    Serial.printf("Current time: %02d:%02d:%02d  %02d/%02d/%04d\n",
                   timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec,
                   timeInfo.tm_mday, timeInfo.tm_mon + 1, timeInfo.tm_year + 1900);
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
      Serial.print("Temperature: ");
      Serial.print(currentTemp, 1);
      Serial.println(" C");
    } else {
      Serial.println("ERROR: DS18B20 not found. Check wiring/resistor.");
    }
    lastTempRead = now;
  }

  // 3. Handle door open/close events (flag set inside the ISR)
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

      // Shift event history: newest goes to index 0
      lastEvents[2] = lastEvents[1];
      lastEvents[1] = lastEvents[0];
      strncpy(lastEvents[0].time, timeStr, sizeof(lastEvents[0].time) - 1);
      lastEvents[0].time[sizeof(lastEvents[0].time) - 1] = '\0';
      lastEvents[0].duration = duration;

      Serial.println("DOOR CLOSED at " + String(timeStr) + ", open for " + String(duration) + "s");
    }
  }

  // 4. Button press -> advance to next screen
  if (buttonPressed) {
    buttonPressed = false;
    currentScreen = (currentScreen + 1) % SCREEN_COUNT;
  }

  // 5. Refresh OLED ~5 times per second
  if (now - lastScreenRefresh > SCREEN_REFRESH_INTERVAL_MS) {
    switch (currentScreen) {
      case 0: drawLiveStatus(); break;
      case 1: drawStats();      break;
      case 2: drawEvents();     break;
      case 3: drawSystem();     break;
    }
    lastScreenRefresh = now;
  }

  // 6. Heartbeat LED blink (1 second on/off) — proves the loop is alive
  if (now - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);  // swap HIGH/LOW if your LED is inverted
    lastHeartbeat = now;
  }
}
