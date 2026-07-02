// ===== Temperature Sensor ====

#include <OneWire.h>
#include <DallasTemperature.h>

#define TEMP_PIN 4   // DS18B20 data wire connected to GPIO 4

OneWire oneWire(TEMP_PIN);          // Create OneWire bus on pin 4
DallasTemperature sensors(&oneWire); // Attach DS18B20 to the bus

void setup() {
  Serial.begin(115200);

  sensors.begin();                  // Start the sensor

  Serial.println("DS18B20 Ready");
}

void loop() {
  sensors.requestTemperatures();    // Ask sensor to take a reading

  // This takes about 750 ms - sensor is converting
  float tempC = sensors.getTempCByIndex(0); // index 0 = first sensor

  if (tempC == DEVICE_DISCONNECTED_C) {     // -127 means wiring problem
    Serial.println("ERROR: Sensor not found. Check wiring and resistor.");
  } 
  else {
    Serial.print("Temperature: ");
    Serial.print(tempC, 1);         // Print with 1 decimal place
    Serial.println(" C");
  }

  delay(2000);                      // Read every 2 seconds
}