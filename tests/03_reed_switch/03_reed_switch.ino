// ===== REED SWITCH =====

#define DOOR_PIN 3
volatile bool doorJustChanged = false;
volatile bool doorIsOpen = false;
volatile unsigned long lastInterruptTime = 0;
unsigned long doorOpenTime = 0;   // track when it opened

void IRAM_ATTR doorISR() {
  unsigned long now = millis();
  if (now - lastInterruptTime < 50) return;
  lastInterruptTime = now;
  doorIsOpen = (digitalRead(DOOR_PIN) == HIGH);
  doorJustChanged = true;
}

void setup() {
  Serial.begin(115200);
  pinMode(DOOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(DOOR_PIN), doorISR, CHANGE);
  Serial.println("Door monitor ready");
}

void loop() {
  if (doorJustChanged) {
    doorJustChanged = false;
    if (doorIsOpen) {
      doorOpenTime = millis();           // remember when it opened
      Serial.println("DOOR OPENED");
    } else {
      unsigned long durationSec = (millis() - doorOpenTime) / 1000;
      Serial.print("DOOR CLOSED — was open for ");
      Serial.print(durationSec);
      Serial.println(" seconds");
    }
  }
}