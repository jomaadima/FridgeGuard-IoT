// =====TELEGRAM BOT=====
//  Connects to WiFi, sends ONE test message to your Telegram bot,
//  and prints the exact result to Serial Monitor so you can tell
//  immediately whether the token/chat ID are correct.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "secrets.h"
// ^ Copy secrets_template.h -> secrets.h in this folder and fill in
//   your own WiFi and Telegram Bot values before uploading. See README.md.

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

// Telegram's URL can't contain raw spaces or punctuation — this converts
// "Hello world!" into "Hello%20world%21" so the server can parse it correctly.
// Without this, any message with a space in it causes a 400 Bad Request.
String urlEncode(String str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;                 // letters/numbers pass through unchanged
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", c);    // everything else becomes %XX hex code
      encoded += buf;
    }
  }
  return encoded;
}

void sendTelegram(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot send — WiFi not connected");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();   // skip SSL cert check (fine for student projects)

  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/sendMessage?chat_id=";
  url += CHAT_ID;
  url += "&text=";
  url += urlEncode(message);   // <-- the actual fix

  http.begin(client, url);
  int httpCode = http.GET();

  Serial.print("HTTP response code: ");
  Serial.println(httpCode);

  if (httpCode == 200) {
    Serial.println("SUCCESS — check your Telegram app now!");
  } else if (httpCode == 401) {
    Serial.println("FAILED — 401 means your BOT_TOKEN is wrong. Re-copy it from BotFather.");
  } else if (httpCode == 400) {
    Serial.println("FAILED — 400 usually means your CHAT_ID is wrong, or you haven't pressed START on the bot yet.");
  } else if (httpCode == -1) {
    Serial.println("FAILED — -1 means a network error (WiFi dropped mid-request, or no internet access).");
  } else {
    Serial.println("FAILED — unexpected code, check Telegram's Bot API error docs for this number.");
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== FridgeGuard Telegram Test ===");

  connectWiFi();

  Serial.println("Sending test message now...");
  sendTelegram("FridgeGuard test message - if you see this, Telegram is working!");
}

void loop() {
  // One-shot test — nothing repeating here.
  // Press the board's RESET button to send the test message again.
}
