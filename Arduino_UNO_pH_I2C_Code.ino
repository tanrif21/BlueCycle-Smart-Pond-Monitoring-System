#include <Wire.h>

#define PH_PIN A0

int rawValue = 0;
float voltage = 0.0;

float neutralVoltage = 2.50;
float voltageDiff = 0.0;

float phValue = 7.0;

// pH sensitivity
float slope = 6.0;

// Neutral range
float threshold = 0.08;

// ======================
// I2C Slave Function
// ======================
void requestEvent() {
  Wire.write((uint8_t*)&phValue, sizeof(phValue));
}

int readAverageRaw() {
  long sum = 0;

  for (int i = 0; i < 80; i++) {
    sum += analogRead(PH_PIN);
    delay(5);
  }

  return sum / 80;
}

void calibrateNeutralWater() {
  Serial.println("================================");
  Serial.println("Keep probe in pH 7 normal water");
  Serial.println("Do not touch the probe");
  Serial.println("Wait 8 seconds...");
  Serial.println("================================");

  delay(8000);

  long sum = 0;

  for (int i = 0; i < 150; i++) {
    sum += analogRead(PH_PIN);
    delay(10);
  }

  rawValue = sum / 150;
  neutralVoltage = rawValue * (5.0 / 1023.0);

  Serial.println();
  Serial.println("Calibration Done!");
  Serial.print("Neutral Raw: ");
  Serial.println(rawValue);
  Serial.print("Neutral Voltage: ");
  Serial.print(neutralVoltage, 3);
  Serial.println(" V");
  Serial.println("This water is now pH 7.00 baseline");
  Serial.println("Now test Acid / Base");
  Serial.println("================================");
}

void setup() {

  Serial.begin(9600);
  pinMode(PH_PIN, INPUT);

  // I2C Slave Address
  Wire.begin(8);
  Wire.onRequest(requestEvent);

  Serial.println("Arduino pH Value + Acid/Base Detector Ready");

  calibrateNeutralWater();
}

void loop() {

  rawValue = readAverageRaw();
  voltage = rawValue * (5.0 / 1023.0);

  voltageDiff = voltage - neutralVoltage;

  // pH estimate
  phValue = 7.0 + ((neutralVoltage - voltage) * slope);

  if (abs(voltageDiff) <= threshold) {
    phValue = 7.00;
  }

  if (phValue < 3.0) phValue = 3.0;
  if (phValue > 11.0) phValue = 11.0;

  Serial.print("Raw: ");
  Serial.print(rawValue);

  Serial.print(" | Voltage: ");
  Serial.print(voltage, 3);
  Serial.print(" V");

  Serial.print(" | NeutralV: ");
  Serial.print(neutralVoltage, 3);
  Serial.print(" V");

  Serial.print(" | Diff: ");
  Serial.print(voltageDiff, 3);
  Serial.print(" V");

  Serial.print(" | pH: ");
  Serial.print(phValue, 2);

  Serial.print(" | Result: ");

  if (voltageDiff > threshold) {
    Serial.println("ACID");
  }
  else if (voltageDiff < -threshold) {
    Serial.println("BASE");
  }
  else {
    Serial.println("NEUTRAL / SAFE");
  }

  delay(1000);
}