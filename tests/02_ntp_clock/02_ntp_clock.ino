// ===== NTP TIME + OLED CLOCK =====

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <WiFi.h>
#include <time.h>  // For NTP and time functions

#include "secrets.h"
// ^ Copy secrets_template.h -> secrets.h in this folder and fill in
//   your own WiFi credentials before uploading. See README.md.

// NTP configuration
const char* NTP_SERVER    = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 3 * 3600;  // UTC+3 for Lebanon
const int   DST_OFFSET_SEC = 0;          // Set to 3600 if DST active

U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println(" Connected!");
}

void syncNTP() {
  // Tell ESP32 the NTP server and your timezone
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);

  struct tm timeInfo;
  // Wait until time is actually synced (timeout 10 seconds)
  int retries = 0;
  while (!getLocalTime(&timeInfo) && retries < 20) {
    delay(500);
    retries++;
  }
  if (retries < 20) {
    Serial.println("NTP sync successful!");
  } else {
    Serial.println("NTP sync FAILED");
  }
}

void drawClock() {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo)) return;  // Skip if time not available

  // Format time as HH:MM:SS
  char timeBuf[9];
  sprintf(timeBuf, "%02d:%02d:%02d",
          timeInfo.tm_hour,
          timeInfo.tm_min,
          timeInfo.tm_sec);

  // Format date as DD/MM/YYYY
  char dateBuf[11];
  sprintf(dateBuf, "%02d/%02d/%04d",
          timeInfo.tm_mday,
          timeInfo.tm_mon + 1,   // tm_mon is 0-indexed (0=January)
          timeInfo.tm_year + 1900); // tm_year is years since 1900

  // Draw to OLED
  u8g2.clearBuffer();

  // Large font for time
  u8g2.setFont(u8g2_font_7x13_tr);
  u8g2.drawStr(2, 17, timeBuf);

  // Smaller font for date
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(5, 32, dateBuf);

  // WiFi indicator dot (top right corner)
  if (WiFi.status() == WL_CONNECTED) {
    u8g2.drawDisc(68, 4, 2);  // Filled circle = connected
  }

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  u8g2.begin();
  u8g2.setContrast(200);
  connectWiFi();
  syncNTP();
}

void loop() {
  drawClock();    // Update display every second
  delay(1000);
}
