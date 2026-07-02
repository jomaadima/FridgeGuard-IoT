// ===== WIFI CONNECTION =====
//later on we used low powerfor a better connnection 
#include "secrets.h"
//^Copy secrets_template.h -> secrets.h in this folder and fill in
//your own WiFi credentials before uploading. See README.md.

#include <WiFi.h>  

#define LED_PIN 8

void setup() {
  Serial.begin(115200);
  delay(2000);
  pinMode(LED_PIN, OUTPUT);

  Serial.println("Starting WiFi connection...");
  Serial.print("Connecting to: ");
  Serial.println(WIFI_SSID);

  // Set to Station mode (connect to existing network)
  WiFi.mode(WIFI_STA);

  // Start connection attempt
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Wait until connected
  // WiFi.status() returns WL_CONNECTED when successful
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts > 40) {  // 20 second timeout
      Serial.println("\nFAILED - check credentials");
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n===== CONNECTED! =====");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());  // Prints e.g. 192.168.1.45
    Serial.print("Signal (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    // Solid LED = connected
    digitalWrite(LED_PIN, LOW);  // LOW = ON on most C3 boards
  }
}

void loop() {
  // Print WiFi status every 5 seconds
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[OK] Connected | IP: ");
    Serial.print(WiFi.localIP());
    Serial.print(" | RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("[!!] WiFi DISCONNECTED");
  }
  delay(5000);
}
