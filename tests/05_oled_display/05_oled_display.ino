// ===== OLED BASICS =====

#include <Arduino.h>
#include <U8g2lib.h>  // The u8g2 display library
#include <Wire.h>     // Required for I2C communication

// Constructor for SSD1306 72x40 display
// Parameters: rotation, SDA pin, SCL pin
// U8G2_R0 = no rotation
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);
//                                              ^RESET  ^SCL ^SDA

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize the display
  u8g2.begin();
  u8g2.setContrast(255);  // Max brightness

  // --- Draw Test Screen ---
  u8g2.clearBuffer();

  // Draw a border rectangle around the full screen
  u8g2.drawFrame(0, 0, 72, 40);  // x, y, width, height

  // Set a small font and draw text
  u8g2.setFont(u8g2_font_5x7_tr);  // 5px wide, 7px tall font
  u8g2.drawStr(10, 15, "ESP32-C3");  // x, y (y is baseline), text
  u8g2.drawStr(10, 28, "Week 2 OK!");

  // Send buffer to display
  u8g2.sendBuffer();

  Serial.println("Display initialized and test screen drawn");
}

void loop() {
  // Display stays static in this example
  // In later weeks, this loop will update every second
  delay(1000);
}
