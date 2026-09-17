#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>

// ================= WIFI =================
const char* AP_NAME = "BlueCycle_Demo";
const char* AP_PASS = "12345678";
WebServer server(80);

// ================= pH I2C FROM ARDUINO UNO =================
TwoWire pHWire = TwoWire(1);
float pH = 7.0;

// ================= LCD =================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ================= SENSOR PINS =================
#define ONE_WIRE_BUS 4
#define TURBIDITY_PIN 35
#define PH_PIN 34

#define TRIG_PIN 18
#define ECHO_PIN 34

#define BUZZER_PIN 23
#define SERVO_PIN 13

// Limit switch pins for 360 feeding servo
// Switch connection: one side to GPIO, other side to GND
#define SERVO_HOME_SWITCH_PIN 16
#define SERVO_TARGET_SWITCH_PIN 17

// ================= RELAY PINS =================
// Relay Active HIGH: HIGH = ON, LOW = OFF
#define RELAY_ACID_PUMP   25   // IN1 = Acid Dosing Pump
#define RELAY_OXYGEN      26   // IN2 = Air / Oxygen Pump
#define RELAY_BASE_PUMP   27   // IN3 = Base Dosing Pump
#define RELAY_WATER_PUMP  14   // IN4 = Water Pump

// ================= 360 SERVO SETTINGS =================
int servoStopValue = 90;
int servoSpeedValue = 35;
int servoDirectionValue = 1;
unsigned long servoRunTimeMs = 1000;

bool servoPulseActive = false;
unsigned long servoPulseStart = 0;
unsigned long servoPulseDuration = 1000;

// ================= SERVO FEEDING SCHEDULE SETTINGS =================
enum FeedCycleState {
  FEED_IDLE,
  FEED_OPENING,
  FEED_WAITING_REVERSE,
  FEED_CLOSING
};

FeedCycleState feedCycleState = FEED_IDLE;

unsigned long feedStageStart = 0;
const unsigned long FEED_REVERSE_DELAY_MS = 15000;

bool feedCyclePending = false;

unsigned long feed1Target = 0;
unsigned long feed2Target = 0;

bool feed1Armed = false;
bool feed2Armed = false;

bool feed1Done = false;
bool feed2Done = false;

// ================= SERVO LIMIT SWITCH SETTINGS =================
// HOME switch = starting point
// TARGET switch = food release point
// INPUT_PULLUP means: pressed = LOW, not pressed = HIGH
bool servoHomePressed = false;
bool servoTargetPressed = false;
const unsigned long SERVO_LIMIT_TIMEOUT_MS = 10000;

// ================= OBJECTS =================
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
Servo myServo;

// ================= REAL VALUES =================
float realTemp = -127.0;
float realPH = 7.0;

int realTurbidity = 0;
int turbidityRaw = 0;
int pondTurbidityBaseline = 0;
bool turbidityCalibrated = false;

float realWaterLevel = 0.0;

// ================= TURBIDITY SETTINGS FOR POND WATER =================
const int TURBIDITY_NORMAL_VALUE = 280;
const int TURBIDITY_SENSITIVITY_RANGE = 250;

// ================= AUTO RANGE SETTINGS =================
float tempLowLimit = 18.0;
float tempHighLimit = 32.0;

float phLowLimit = 6.5;
float phHighLimit = 8.5;

int turbidityLimit = 550;

// ================= ULTRASONIC WATER LEVEL SETTINGS =================
// Safe center = 15 cm
// < 14 cm  = OVERFLOW, display only, pump OFF
// 14-16 cm = SAFE
// > 16 cm  = UNDERFLOW, Auto mode e water pump ON
float waterLevelLimit = 16.0;

const float BOX_HEIGHT_CM = 18.0;
float waterSafeDistance = 14.0;
float waterUnderflowDistance = 16.0;

String waterLevelStatus = "SAFE";
bool waterAutoMode = true;

// Auto pulse cooldown
unsigned long lastAutoActionTime = 0;
const unsigned long AUTO_COOLDOWN = 6000;

// ================= DEMO VALUES =================
bool demoMode = true;

float demoTemp = 26.5;
float demoPH = 7.0;
int demoTurbidity = 180;
float demoWaterLevel = 15.0;

// ================= DISPLAY VALUES =================
float showTemp = 26.5;
float showPH = 7.0;
int showTurbidity = 180;
float showWaterLevel = 15.0;

// ================= PUMP STATUS =================
bool acidPumpStatus = false;
bool basePumpStatus = false;
bool waterPumpStatus = false;
bool oxygenPumpStatus = false;

// ================= PULSE STATUS =================
bool acidPulseActive = false;
bool basePulseActive = false;
bool waterPulseActive = false;
bool oxygenPulseActive = false;

unsigned long acidPulseStart = 0;
unsigned long basePulseStart = 0;
unsigned long waterPulseStart = 0;
unsigned long oxygenPulseStart = 0;

const unsigned long PULSE_TIME = 3000;

// ================= TIMING =================
unsigned long lastSensorRead = 0;
unsigned long lastLCD = 0;
int lcdPage = 0;

// ================= BASIC FUNCTIONS =================

void relayOn(int pin) {
  digitalWrite(pin, HIGH);
}

void relayOff(int pin) {
  digitalWrite(pin, LOW);
}

// ================= BUZZER ALERT =================

void buzzerBeep() {
  tone(BUZZER_PIN, 2000);
  delay(300);
  noTone(BUZZER_PIN);
}

// ================= WATER LEVEL STATUS =================

void updateWaterLevelStatus() {
  if (showWaterLevel <= 0) {
    waterLevelStatus = "NO READING";
  }
  else if (showWaterLevel < waterSafeDistance) {
    waterLevelStatus = "OVERFLOW";
  }
  else if (showWaterLevel > waterUnderflowDistance) {
    waterLevelStatus = "UNDERFLOW";
  }
  else {
    waterLevelStatus = "SAFE";
  }
}

void updateWaterPumpAuto() {
  updateWaterLevelStatus();

  if (!waterAutoMode) {
    return;
  }

  // Overflow = display only, pump OFF
  // Underflow = pump ON
  if (waterLevelStatus == "UNDERFLOW") {
    relayOn(RELAY_WATER_PUMP);
    waterPumpStatus = true;
    waterPulseActive = false;
    Serial.println("WATER AUTO: UNDERFLOW -> Water Pump ON");
  }
  else {
    relayOff(RELAY_WATER_PUMP);
    waterPumpStatus = false;
    waterPulseActive = false;

    if (waterLevelStatus == "OVERFLOW") {
      Serial.println("WATER AUTO: OVERFLOW -> Display only, Pump OFF");
    }
  }
}

// ================= 360 SERVO CONTROL =================

void servoAttachIfNeeded() {
  if (!myServo.attached()) {
    myServo.attach(SERVO_PIN);
    delay(50);
  }
}

void servoStop360() {
  if (myServo.attached()) {
    myServo.write(servoStopValue);
    delay(200);
    myServo.detach();
  }

  servoPulseActive = false;
  Serial.println("Servo STOP");
}

void servoRun360(int speedValue, int directionValue) {
  servoAttachIfNeeded();

  if (speedValue < 0) speedValue = 0;
  if (speedValue > 90) speedValue = 90;

  int servoWriteValue;

  if (directionValue >= 0) {
    servoWriteValue = servoStopValue + speedValue;
  } else {
    servoWriteValue = servoStopValue - speedValue;
  }

  servoWriteValue = constrain(servoWriteValue, 0, 180);

  myServo.write(servoWriteValue);

  Serial.print("Servo Run | Speed: ");
  Serial.print(speedValue);
  Serial.print(" | Direction: ");
  Serial.print(directionValue >= 0 ? "Forward" : "Reverse");
  Serial.print(" | Write Value: ");
  Serial.println(servoWriteValue);
}

void startServoPulse(int speedValue, int directionValue, unsigned long durationMs) {
  servoSpeedValue = speedValue;
  servoDirectionValue = directionValue;
  servoRunTimeMs = durationMs;

  if (servoSpeedValue < 0) servoSpeedValue = 0;
  if (servoSpeedValue > 90) servoSpeedValue = 90;

  if (servoRunTimeMs < 100) servoRunTimeMs = 100;
  if (servoRunTimeMs > 10000) servoRunTimeMs = 10000;

  servoRun360(servoSpeedValue, servoDirectionValue);

  servoPulseStart = millis();
  servoPulseDuration = servoRunTimeMs;
  servoPulseActive = true;
}

void updateServoPulse() {
  if (servoPulseActive && millis() - servoPulseStart >= servoPulseDuration) {
    servoStop360();
  }
}

bool isServoHomePressed() {
  return digitalRead(SERVO_HOME_SWITCH_PIN) == LOW;
}

bool isServoTargetPressed() {
  return digitalRead(SERVO_TARGET_SWITCH_PIN) == LOW;
}

void updateServoLimitSwitches() {
  servoHomePressed = isServoHomePressed();
  servoTargetPressed = isServoTargetPressed();
}

void startFeedingCycle();

void finishFeedingCycle() {
  servoStop360();
  feedCycleState = FEED_IDLE;

  Serial.println("FEEDING: Complete, servo returned to home");

  if (feedCyclePending) {
    feedCyclePending = false;
    startFeedingCycle();
  }
}

// ================= FEEDING SCHEDULE SERVO CONTROL =================

void startFeedingCycle() {
  if (feedCycleState != FEED_IDLE) {
    feedCyclePending = true;
    Serial.println("Feeding already running, next feeding pending");
    return;
  }

  updateServoLimitSwitches();
  servoPulseActive = false;

  if (servoTargetPressed) {
    Serial.println("FEEDING: Target switch already pressed, waiting before reverse");
    servoStop360();
    feedStageStart = millis();
    feedCycleState = FEED_WAITING_REVERSE;
    return;
  }

  servoRun360(servoSpeedValue, 1);
  feedStageStart = millis();
  feedCycleState = FEED_OPENING;

  Serial.println("FEEDING: Moving to TARGET switch");
}

void updateFeedingCycle() {
  unsigned long now = millis();
  updateServoLimitSwitches();

  if (feed1Armed && !feed1Done && now >= feed1Target) {
    startFeedingCycle();
    feed1Done = true;
    Serial.println("Feed 1 time reached");
  }

  if (feed2Armed && !feed2Done && now >= feed2Target) {
    startFeedingCycle();
    feed2Done = true;
    Serial.println("Feed 2 time reached");
  }

  if (feedCycleState == FEED_OPENING) {
    if (servoTargetPressed) {
      servoStop360();
      feedStageStart = now;
      feedCycleState = FEED_WAITING_REVERSE;

      Serial.println("FEEDING: TARGET switch pressed, waiting 15 sec before reverse");
    }
    else if (now - feedStageStart >= SERVO_LIMIT_TIMEOUT_MS) {
      servoStop360();
      feedCycleState = FEED_IDLE;
      Serial.println("FEEDING ERROR: TARGET switch not found, servo stopped");
    }
  }

  else if (feedCycleState == FEED_WAITING_REVERSE) {
    if (now - feedStageStart >= FEED_REVERSE_DELAY_MS) {
      updateServoLimitSwitches();

      if (servoHomePressed) {
        finishFeedingCycle();
      }
      else {
        servoRun360(servoSpeedValue, -1);
        feedStageStart = now;
        feedCycleState = FEED_CLOSING;

        Serial.println("FEEDING: Returning to HOME switch");
      }
    }
  }

  else if (feedCycleState == FEED_CLOSING) {
    if (servoHomePressed) {
      finishFeedingCycle();
    }
    else if (now - feedStageStart >= SERVO_LIMIT_TIMEOUT_MS) {
      servoStop360();
      feedCycleState = FEED_IDLE;
      Serial.println("FEEDING ERROR: HOME switch not found, servo stopped");
    }
  }
}

String getFeedStatusText() {
  if (feedCycleState == FEED_OPENING) return "MOVING TO TARGET";
  if (feedCycleState == FEED_WAITING_REVERSE) return "WAITING 15 SEC";
  if (feedCycleState == FEED_CLOSING) return "RETURNING HOME";
  if (feedCyclePending) return "PENDING";
  return "IDLE";
}

long remainingSeconds(unsigned long target, bool armed, bool done) {
  if (!armed || done) return -1;

  unsigned long now = millis();

  if (now >= target) return 0;

  return (long)((target - now) / 1000);
}

void allPumpsOff() {
  relayOff(RELAY_ACID_PUMP);
  relayOff(RELAY_BASE_PUMP);
  relayOff(RELAY_WATER_PUMP);
  relayOff(RELAY_OXYGEN);

  acidPumpStatus = false;
  basePumpStatus = false;
  waterPumpStatus = false;
  oxygenPumpStatus = false;

  acidPulseActive = false;
  basePulseActive = false;
  waterPulseActive = false;
  oxygenPulseActive = false;

  feedCycleState = FEED_IDLE;
  feedCyclePending = false;

  servoStop360();

  Serial.println("ALL PUMPS OFF");
}

// ================= SENSOR READ =================

float readDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 0;

  return duration * 0.034 / 2.0;
}

int readAverageAnalog(int pin) {
  long sum = 0;

  for (int i = 0; i < 20; i++) {
    sum += analogRead(pin);
    delay(3);
  }

  return sum / 20;
}

void calibrateTurbidityPondWater() {
  Serial.println("==================================");
  Serial.println("Turbidity pond-water calibration");
  Serial.println("Keep sensor in NORMAL POND WATER");
  Serial.println("Calibration will start in 5 seconds");
  Serial.println("==================================");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Turb Calib");
  lcd.setCursor(0, 1);
  lcd.print("Pond Water");

  delay(5000);

  long sum = 0;

  for (int i = 0; i < 100; i++) {
    sum += analogRead(TURBIDITY_PIN);
    delay(10);
  }

  pondTurbidityBaseline = sum / 100;
  turbidityCalibrated = true;

  Serial.print("Pond Water Baseline Raw: ");
  Serial.println(pondTurbidityBaseline);
  Serial.println("Normal pond water display value starts near 280");
  Serial.println("==================================");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Pond Base:");
  lcd.print(pondTurbidityBaseline);
  lcd.setCursor(0, 1);
  lcd.print("Calib Done");
  delay(1000);
}

void readPHFromUNO() {
  pHWire.requestFrom(8, sizeof(pH));

  if (pHWire.available() >= sizeof(pH)) {
    pHWire.readBytes((char*)&pH, sizeof(pH));

    if (pH >= 0.0 && pH <= 14.0) {
      realPH = pH;
    }

    Serial.print("pH Value: ");
    Serial.println(realPH, 2);
  }
  else {
    Serial.println("No pH data received");
  }
}

void readSensors() {
  tempSensor.requestTemperatures();
  realTemp = tempSensor.getTempCByIndex(0);

  readPHFromUNO();

  turbidityRaw = readAverageAnalog(TURBIDITY_PIN);

  if (turbidityCalibrated) {
    int changeAmount = abs(turbidityRaw - pondTurbidityBaseline);

    int extraTurbidity = map(changeAmount, 0, TURBIDITY_SENSITIVITY_RANGE, 0, 720);

    if (extraTurbidity < 0) extraTurbidity = 0;
    if (extraTurbidity > 720) extraTurbidity = 720;

    realTurbidity = TURBIDITY_NORMAL_VALUE + extraTurbidity;

    if (realTurbidity < 0) realTurbidity = 0;
    if (realTurbidity > 1000) realTurbidity = 1000;
  }
  else {
    realTurbidity = TURBIDITY_NORMAL_VALUE;
  }

  Serial.print("Turbidity Raw: ");
  Serial.print(turbidityRaw);
  Serial.print(" | Pond Baseline: ");
  Serial.print(pondTurbidityBaseline);
  Serial.print(" | Difference: ");
  Serial.print(abs(turbidityRaw - pondTurbidityBaseline));
  Serial.print(" | Pond Turbidity Value: ");
  Serial.println(realTurbidity);

  realWaterLevel = readDistanceCM();

  if (demoMode) {
    showTemp = demoTemp;
    showPH = demoPH;
    showTurbidity = demoTurbidity;
    showWaterLevel = demoWaterLevel;
  } else {
    showTemp = realTemp;
    showPH = realPH;
    showTurbidity = realTurbidity;
    showWaterLevel = realWaterLevel;
  }

  updateWaterLevelStatus();
}

// ================= PULSE UPDATE =================

void updatePulses() {
  unsigned long now = millis();

  if (acidPulseActive && now - acidPulseStart >= PULSE_TIME) {
    relayOff(RELAY_ACID_PUMP);
    acidPumpStatus = false;
    acidPulseActive = false;
    Serial.println("Acid Pump OFF");
  }

  if (basePulseActive && now - basePulseStart >= PULSE_TIME) {
    relayOff(RELAY_BASE_PUMP);
    basePumpStatus = false;
    basePulseActive = false;
    Serial.println("Base Pump OFF");
  }

  if (waterPulseActive && now - waterPulseStart >= PULSE_TIME) {
    relayOff(RELAY_WATER_PUMP);
    waterPumpStatus = false;
    waterPulseActive = false;
    Serial.println("Water Pump OFF");
  }

  if (oxygenPulseActive && now - oxygenPulseStart >= PULSE_TIME) {
    relayOff(RELAY_OXYGEN);
    oxygenPumpStatus = false;
    oxygenPulseActive = false;
    Serial.println("Oxygen Pump OFF");
  }
}

// ================= AUTOMATION ACTIONS =================

void stopAcidBasePumps() {
  relayOff(RELAY_ACID_PUMP);
  relayOff(RELAY_BASE_PUMP);

  acidPumpStatus = false;
  basePumpStatus = false;

  acidPulseActive = false;
  basePulseActive = false;
}

void startAcidPulse() {
  stopAcidBasePumps();

  relayOn(RELAY_ACID_PUMP);
  acidPumpStatus = true;
  acidPulseActive = true;
  acidPulseStart = millis();

  Serial.println("AUTO: BASIC pH -> Acid Pump Pulse");
}

void startBasePulse() {
  stopAcidBasePumps();

  relayOn(RELAY_BASE_PUMP);
  basePumpStatus = true;
  basePulseActive = true;
  basePulseStart = millis();

  Serial.println("AUTO: ACIDIC pH -> Base Pump Pulse");
}

void startWaterPulse() {
  if (waterAutoMode && waterLevelStatus == "UNDERFLOW") {
    relayOn(RELAY_WATER_PUMP);
    waterPumpStatus = true;
    waterPulseActive = false;
    Serial.println("AUTO: UNDERFLOW -> Water Pump Continuous ON");
    return;
  }

  relayOn(RELAY_WATER_PUMP);
  waterPumpStatus = true;
  waterPulseActive = true;
  waterPulseStart = millis();

  Serial.println("AUTO: Water Pump Pulse");
}

void startOxygenPulse() {
  relayOn(RELAY_OXYGEN);
  oxygenPumpStatus = true;
  oxygenPulseActive = true;
  oxygenPulseStart = millis();

  Serial.println("AUTO: High Temp -> Oxygen Pump Pulse");
}

// ================= REAL AUTOMATION LOGIC =================

void runRealAutomation() {
  if (demoMode) return;

  updateWaterLevelStatus();
  updateWaterPumpAuto();

  unsigned long now = millis();

  if (now - lastAutoActionTime < AUTO_COOLDOWN) {
    return;
  }

  bool actionTaken = false;

  if (showPH < phLowLimit) {
    startBasePulse();
    actionTaken = true;
    Serial.println("REAL AUTO: pH LOW -> Base Pump");
  }
  else if (showPH > phHighLimit) {
    startAcidPulse();
    actionTaken = true;
    Serial.println("REAL AUTO: pH HIGH -> Acid Pump");
  }
  else {
    stopAcidBasePumps();
  }

  if (showTemp > tempHighLimit) {
    startOxygenPulse();
    actionTaken = true;
    Serial.println("REAL AUTO: High Temp -> Oxygen Pump");
  }

  if (showTurbidity > turbidityLimit) {
    startWaterPulse();
    actionTaken = true;
    Serial.println("REAL AUTO: Dirty Water -> Water Pump");
  }

  if (waterLevelStatus == "UNDERFLOW") {
    actionTaken = true;
    Serial.println("REAL AUTO: UNDERFLOW detected");
  }

  if (waterLevelStatus == "OVERFLOW") {
    Serial.println("REAL AUTO: OVERFLOW display only, pump OFF");
  }

  if (actionTaken) {
    buzzerBeep();
    lastAutoActionTime = now;
  }
}

// ================= DEMO AUTOMATION LOGIC =================

void runDemoAutomation() {
  updateWaterLevelStatus();

  Serial.println("========== DEMO AUTOMATION ==========");
  Serial.print("Demo Temp: ");
  Serial.println(demoTemp);

  Serial.print("Demo pH: ");
  Serial.println(demoPH);

  Serial.print("Demo Turbidity: ");
  Serial.println(demoTurbidity);

  Serial.print("Demo Water Distance: ");
  Serial.println(demoWaterLevel);

  Serial.print("Water Status: ");
  Serial.println(waterLevelStatus);

  bool demoAlert = false;

  if (demoPH < phLowLimit) {
    startBasePulse();
    demoAlert = true;
  }
  else if (demoPH > phHighLimit) {
    startAcidPulse();
    demoAlert = true;
  }
  else {
    Serial.println("AUTO: pH SAFE -> No Acid/Base Pump");
    stopAcidBasePumps();
  }

  if (demoTemp > tempHighLimit) {
    startOxygenPulse();
    demoAlert = true;
  }

  if (demoTurbidity > turbidityLimit) {
    startWaterPulse();
    demoAlert = true;
  }

  if (waterLevelStatus == "UNDERFLOW" || waterLevelStatus == "OVERFLOW") {
    demoAlert = true;
  }

  updateWaterPumpAuto();

  if (demoAlert) {
    buzzerBeep();
  }

  Serial.println("=====================================");
}

// ================= LCD =================

void updateLCD() {
  lcd.clear();

  if (lcdPage == 0) {
    lcd.setCursor(0, 0);
    lcd.print(demoMode ? "MODE: DEMO" : "MODE: REAL");

    lcd.setCursor(0, 1);
    lcd.print("IP:192.168.4.1");
  }

  else if (lcdPage == 1) {
    lcd.setCursor(0, 0);
    lcd.print("Temp:");
    lcd.print(showTemp, 1);
    lcd.print("C");

    lcd.setCursor(0, 1);
    lcd.print("pH:");
    lcd.print(showPH, 1);
  }

  else if (lcdPage == 2) {
    lcd.setCursor(0, 0);
    lcd.print("Turb:");
    lcd.print(showTurbidity);

    lcd.setCursor(0, 1);
    lcd.print("Dist:");
    lcd.print(showWaterLevel, 1);
    lcd.print("cm");
  }

  else if (lcdPage == 3) {
    lcd.setCursor(0, 0);
    lcd.print("Water:");
    lcd.print(waterLevelStatus);

    lcd.setCursor(0, 1);
    lcd.print("Pump:");
    lcd.print(waterPumpStatus ? "ON " : "OFF");
    lcd.print(waterAutoMode ? " A" : " M");
  }

  else {
    lcd.setCursor(0, 0);
    lcd.print("Feed:");
    lcd.print(getFeedStatusText());

    lcd.setCursor(0, 1);
    long r1 = remainingSeconds(feed1Target, feed1Armed, feed1Done);
    lcd.print("F1:");
    if (r1 < 0) lcd.print("--");
    else lcd.print(r1);
    lcd.print("s");
  }

  lcdPage++;
  if (lcdPage > 4) lcdPage = 0;
}

// ================= WEB HELPERS =================

String boolText(bool v) {
  return v ? "true" : "false";
}

// ================= DASHBOARD =================

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>BlueCycle Dashboard</title>

<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{
  margin:0;
  font-family:Arial,Helvetica,sans-serif;
  background:#eef7fb;
  color:#102a43;
}
.app{
  max-width:1200px;
  margin:auto;
  padding:14px;
}
.header{
  background:linear-gradient(135deg,#0057d9,#00a884);
  color:white;
  padding:22px;
  border-radius:24px;
  box-shadow:0 10px 25px rgba(0,0,0,.18);
}
.header h1{margin:0;font-size:28px}
.header p{margin:8px 0 0;font-size:14px;opacity:.95}
.topbar{
  display:flex;
  gap:10px;
  flex-wrap:wrap;
  margin-top:14px;
}
.pill{
  background:rgba(255,255,255,.2);
  padding:8px 12px;
  border-radius:30px;
  font-size:13px;
  font-weight:bold;
}
.grid4{
  display:grid;
  grid-template-columns:repeat(4,1fr);
  gap:14px;
  margin-top:14px;
}
.grid2{
  display:grid;
  grid-template-columns:repeat(2,1fr);
  gap:14px;
  margin-top:14px;
}
.card{
  background:white;
  border-radius:22px;
  padding:18px;
  box-shadow:0 8px 22px rgba(0,0,0,.10);
  border:1px solid #e5eef5;
}
.title{
  color:#66788a;
  font-size:13px;
  font-weight:bold;
  text-transform:uppercase;
}
.value{
  font-size:36px;
  font-weight:900;
  margin-top:8px;
}
.real{
  color:#64748b;
  font-size:13px;
  margin-top:7px;
}
.badge{
  display:inline-block;
  padding:7px 12px;
  border-radius:20px;
  margin-top:10px;
  color:white;
  font-weight:bold;
  font-size:12px;
}
.ok{background:#16a34a}
.bad{background:#ef4444}
.warn{background:#f59e0b}
.off{background:#64748b}
.bluebadge{background:#2563eb}
h2{margin:0 0 8px}
.sub{color:#64748b;font-size:13px;margin:0 0 14px}
.formgrid{
  display:grid;
  grid-template-columns:repeat(4,1fr);
  gap:12px;
}
label{
  font-weight:bold;
  font-size:13px;
  color:#334155;
}
input{
  width:100%;
  padding:12px;
  border-radius:13px;
  border:1px solid #cbd5e1;
  margin-top:6px;
  font-size:17px;
  font-weight:bold;
}
button{
  border:none;
  border-radius:14px;
  padding:13px;
  font-size:15px;
  font-weight:900;
  color:white;
  cursor:pointer;
  box-shadow:0 6px 14px rgba(0,0,0,.12);
}
button:active{transform:scale(.97)}
.green{background:#16a34a}
.red{background:#ef4444}
.blue{background:#2563eb}
.orange{background:#f59e0b}
.gray{background:#475569}
.purple{background:#7c3aed}
.dark{background:#0f172a}
.btnrow{
  display:grid;
  grid-template-columns:1fr 1fr 1fr;
  gap:9px;
  margin-top:12px;
}
.btnrow2{
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:9px;
}
.pumphead{
  display:flex;
  justify-content:space-between;
  align-items:center;
}
.pumpname{
  font-size:20px;
  font-weight:900;
}
.pin{
  font-size:13px;
  color:#64748b;
  margin-top:3px;
}
.pumpstatus{
  padding:8px 13px;
  color:white;
  border-radius:20px;
  font-weight:900;
}
.toast{
  display:none;
  position:fixed;
  bottom:18px;
  left:50%;
  transform:translateX(-50%);
  background:#0f172a;
  color:white;
  padding:12px 18px;
  border-radius:25px;
  font-weight:bold;
}
.modeBox{
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:10px;
  margin-top:10px;
}
@media(max-width:900px){
  .grid4{grid-template-columns:1fr 1fr}
  .grid2{grid-template-columns:1fr}
  .formgrid{grid-template-columns:1fr 1fr}
}
@media(max-width:520px){
  .grid4{grid-template-columns:1fr}
  .formgrid{grid-template-columns:1fr}
  .btnrow{grid-template-columns:1fr}
  .btnrow2{grid-template-columns:1fr}
  .modeBox{grid-template-columns:1fr}
  .header h1{font-size:23px}
}
</style>
</head>

<body>
<div class="app">

  <div class="header">
    <h1>BlueCycle Smart Pond Control Panel</h1>
    <p>Real Sensor Monitoring + Manual Demo Value Control + Scheduled Feeding</p>
    <div class="topbar">
      <div class="pill">WiFi: BlueCycle_Demo</div>
      <div class="pill">IP: 192.168.4.1</div>
      <div class="pill" id="modeTop">Mode: --</div>
      <div class="pill" id="waterTop">Water: --</div>
    </div>
  </div>

  <div class="grid4">
    <div class="card">
      <div class="title">Temperature</div>
      <div class="value" id="temp">--</div>
      <div class="real" id="realTemp">Real: --</div>
      <span id="tempBadge" class="badge ok">SAFE</span>
    </div>

    <div class="card">
      <div class="title">pH Value</div>
      <div class="value" id="ph">--</div>
      <div class="real" id="realPH">Real: --</div>
      <span id="phBadge" class="badge ok">SAFE</span>
    </div>

    <div class="card">
      <div class="title">Turbidity</div>
      <div class="value" id="turb">--</div>
      <div class="real" id="realTurb">Real: --</div>
      <span id="turbBadge" class="badge ok">OK</span>
    </div>

    <div class="card">
      <div class="title">Ultrasonic Distance</div>
      <div class="value" id="level">--</div>
      <div class="real" id="realLevel">Real: --</div>
      <span id="levelBadge" class="badge ok">OK</span>
    </div>
  </div>

  <div class="grid2">
    <div class="card">
      <h2>Manual Demo Values</h2>
      <p class="sub">Sir ke demo dekhate value input dao, then Apply Demo Values press koro. Real value niche thakbe.</p>

      <div class="formgrid">
        <div>
          <label>Temperature C</label>
          <input id="inTemp" type="number" step="0.1" value="26.5">
        </div>

        <div>
          <label>pH Value</label>
          <input id="inPH" type="number" step="0.1" value="7.0">
        </div>

        <div>
          <label>Turbidity</label>
          <input id="inTurb" type="number" step="1" value="180">
        </div>

        <div>
          <label>Ultrasonic Distance cm</label>
          <input id="inLevel" type="number" step="0.1" value="15.0">
        </div>
      </div>

      <div class="btnrow2" style="margin-top:14px">
        <button class="green" onclick="applyDemo()">Apply Demo Values</button>
        <button class="blue" onclick="cmd('/mode?demo=0','Real sensor mode')">Use Real Sensors</button>
      </div>

      <div class="modeBox">
        <button class="orange" onclick="setQuick(35,7,180,15)">High Temp Demo</button>
        <button class="red" onclick="setQuick(26,5.5,180,15)">Acidic Demo</button>
        <button class="blue" onclick="setQuick(26,9.2,180,15)">Basic Demo</button>
        <button class="purple" onclick="setQuick(26,7,800,17)">Dirty / Underflow</button>
      </div>
    </div>

    <div class="card">
      <h2>Emergency / System</h2>
      <p class="sub">Emergency OFF dile sob pump bondho hobe.</p>

      <div class="btnrow2">
        <button class="dark" onclick="cmd('/alloff','All pumps OFF')">ALL PUMPS OFF</button>
        <button class="purple" onclick="runServo(1)">360 Servo Forward</button>
      </div>

      <div style="margin-top:14px">
        <span id="modeBadge" class="badge bluebadge">MODE</span>
      </div>

      <div style="margin-top:18px">
        <h2>Scheduled Fish Feeding</h2>
        <p class="sub">Minute from now dao. Time hole servo food release korbe, 15 sec wait kore reverse ghurbe.</p>

        <div class="formgrid">
          <div>
            <label>Feed 1 after min</label>
            <input id="feed1Min" type="number" step="1" value="1">
          </div>

          <div>
            <label>Feed 2 after min</label>
            <input id="feed2Min" type="number" step="1" value="2">
          </div>
        </div>

        <div class="btnrow" style="margin-top:14px">
          <button class="green" onclick="setFeedSchedule()">Set Feed Times</button>
          <button class="purple" onclick="cmd('/feednow','Feeding now')">Feed Now</button>
          <button class="gray" onclick="cmd('/clearfeed','Feed timers cleared')">Clear</button>
        </div>

        <p class="sub" style="margin-top:12px">
          Feed Status: <b id="feedStatus">--</b><br>
          Feed 1 Remaining: <b id="feed1Remain">--</b><br>
          Feed 2 Remaining: <b id="feed2Remain">--</b><br>
          Home Switch: <b id="homeSwitch">--</b><br>
          Target Switch: <b id="targetSwitch">--</b>
        </p>
      </div>

      <div style="margin-top:18px">
        <h2>360 Servo Manual Control</h2>
        <p class="sub">Speed + time diye 360 servo range control koro. 90 stop na hole Stop Value 88/92/95 try koro.</p>

        <div class="formgrid">
          <div>
            <label>Servo Speed</label>
            <input id="servoSpeed" type="number" step="1" value="35">
          </div>

          <div>
            <label>Run Time ms</label>
            <input id="servoTime" type="number" step="100" value="1000">
          </div>

          <div>
            <label>Stop Value</label>
            <input id="servoStop" type="number" step="1" value="90">
          </div>
        </div>

        <div class="btnrow" style="margin-top:14px">
          <button class="purple" onclick="runServo(1)">Forward</button>
          <button class="orange" onclick="runServo(-1)">Reverse</button>
          <button class="gray" onclick="cmd('/servostop','Servo Stop')">Stop</button>
        </div>
      </div>

      <div style="margin-top:18px">
        <h2>Auto Range Settings</h2>
        <p class="sub">Water safe center 15 cm. Below 14 cm = OVERFLOW display only. Above 16 cm = UNDERFLOW + pump ON.</p>

        <div class="formgrid">
          <div>
            <label>Temp Low</label>
            <input id="tempLow" type="number" step="0.1" value="18">
          </div>

          <div>
            <label>Temp High</label>
            <input id="tempHigh" type="number" step="0.1" value="32">
          </div>

          <div>
            <label>pH Low</label>
            <input id="phLow" type="number" step="0.1" value="6.5">
          </div>

          <div>
            <label>pH High</label>
            <input id="phHigh" type="number" step="0.1" value="8.5">
          </div>

          <div>
            <label>Turbidity Limit</label>
            <input id="turbLimit" type="number" step="1" value="550">
          </div>

          <div>
            <label>Overflow Below cm</label>
            <input id="waterSafe" type="number" step="0.1" value="14">
          </div>

          <div>
            <label>Underflow Above cm</label>
            <input id="waterUnder" type="number" step="0.1" value="16">
          </div>
        </div>

        <div class="btnrow2" style="margin-top:14px">
          <button class="green" onclick="applyRange()">Apply Range</button>
          <button class="dark" onclick="cmd('/alloff','All pumps OFF')">Emergency Stop</button>
        </div>
      </div>
    </div>
  </div>

  <div class="grid2">
    <div class="card">
      <div class="pumphead">
        <div>
          <div class="pumpname">Acid Dosing Pump</div>
          <div class="pin">Relay IN1 / GPIO25</div>
        </div>
        <span id="acidStatus" class="pumpstatus off">OFF</span>
      </div>
      <div class="btnrow">
        <button class="red" onclick="cmd('/acidpulse','Acid pulse')">PULSE</button>
        <button class="green" onclick="cmd('/acidon','Acid ON')">ON</button>
        <button class="gray" onclick="cmd('/acidoff','Acid OFF')">OFF</button>
      </div>
    </div>

    <div class="card">
      <div class="pumphead">
        <div>
          <div class="pumpname">Base Dosing Pump</div>
          <div class="pin">Relay IN3 / GPIO27</div>
        </div>
        <span id="baseStatus" class="pumpstatus off">OFF</span>
      </div>
      <div class="btnrow">
        <button class="blue" onclick="cmd('/basepulse','Base pulse')">PULSE</button>
        <button class="green" onclick="cmd('/baseon','Base ON')">ON</button>
        <button class="gray" onclick="cmd('/baseoff','Base OFF')">OFF</button>
      </div>
    </div>

    <div class="card">
      <div class="pumphead">
        <div>
          <div class="pumpname">Water Pump</div>
          <div class="pin">Relay IN4 / GPIO14</div>
          <div class="pin">Auto: UNDERFLOW hole pump ON, OVERFLOW just display</div>
        </div>
        <span id="waterStatus" class="pumpstatus off">OFF</span>
      </div>

      <div style="margin-top:8px">
        <span id="waterModeBadge" class="badge bluebadge">AUTO</span>
        <span id="waterLevelStatusBadge" class="badge ok">SAFE</span>
      </div>

      <div class="btnrow">
        <button class="green" onclick="cmd('/waterauto','Water Auto Mode')">AUTO</button>
        <button class="blue" onclick="cmd('/wateron','Manual Water ON')">MANUAL ON</button>
        <button class="gray" onclick="cmd('/wateroff','Manual Water OFF')">MANUAL OFF</button>
      </div>

      <div class="btnrow2">
        <button class="orange" onclick="cmd('/waterpulse','Water pulse')">PULSE</button>
        <button class="dark" onclick="cmd('/watermanual','Manual Mode')">MANUAL MODE</button>
      </div>
    </div>

    <div class="card">
      <div class="pumphead">
        <div>
          <div class="pumpname">Air / Oxygen Pump</div>
          <div class="pin">Relay IN2 / GPIO26</div>
        </div>
        <span id="oxygenStatus" class="pumpstatus off">OFF</span>
      </div>
      <div class="btnrow">
        <button class="green" onclick="cmd('/oxygenon','Air ON')">ON</button>
        <button class="gray" onclick="cmd('/oxygenoff','Air OFF')">OFF</button>
        <button class="orange" onclick="cmd('/oxygenpulse','Air pulse')">PULSE</button>
      </div>
    </div>
  </div>

</div>

<div id="toast" class="toast">Done</div>

<script>
let toastTimer;

function showToast(msg){
  let t=document.getElementById("toast");
  t.innerHTML=msg;
  t.style.display="block";
  clearTimeout(toastTimer);
  toastTimer=setTimeout(()=>{t.style.display="none"},1200);
}

async function cmd(url,msg){
  showToast(msg);
  try{
    await fetch(url);
    setTimeout(load,300);
  }catch(e){
    showToast("Connection problem");
  }
}

async function applyDemo(){
  let t=document.getElementById("inTemp").value;
  let ph=document.getElementById("inPH").value;
  let tu=document.getElementById("inTurb").value;
  let wl=document.getElementById("inLevel").value;

  let url="/setdemo?t="+t+"&ph="+ph+"&tu="+tu+"&wl="+wl;
  await cmd(url,"Demo values applied + automation");
}

async function setFeedSchedule(){
  let f1=document.getElementById("feed1Min").value;
  let f2=document.getElementById("feed2Min").value;

  let url="/setfeed?f1="+f1+"&f2="+f2;
  await cmd(url,"Feed schedule set");
}

async function applyRange(){
  let tl = document.getElementById("tempLow").value;
  let th = document.getElementById("tempHigh").value;

  let pl = document.getElementById("phLow").value;
  let ph = document.getElementById("phHigh").value;

  let tu = document.getElementById("turbLimit").value;

  let ws = document.getElementById("waterSafe").value;
  let wu = document.getElementById("waterUnder").value;

  let url = "/setrange?tl=" + tl + "&th=" + th + "&pl=" + pl + "&ph=" + ph + "&tu=" + tu + "&ws=" + ws + "&wu=" + wu;

  await cmd(url, "Auto range updated");
}

async function runServo(direction){
  let speed = document.getElementById("servoSpeed").value;
  let time = document.getElementById("servoTime").value;
  let stop = document.getElementById("servoStop").value;

  let url = "/servo?s=" + speed + "&time=" + time + "&stop=" + stop + "&d=" + direction;

  await cmd(url, direction == 1 ? "Servo Forward" : "Servo Reverse");
}

function setQuick(t,ph,tu,wl){
  document.getElementById("inTemp").value=t;
  document.getElementById("inPH").value=ph;
  document.getElementById("inTurb").value=tu;
  document.getElementById("inLevel").value=wl;
  applyDemo();
}

function setBadge(id,text,cls){
  let e=document.getElementById(id);
  e.innerHTML=text;
  e.className="badge "+cls;
}

function setPump(id,state){
  let e=document.getElementById(id);
  e.innerHTML=state?"ON":"OFF";
  e.className=state?"pumpstatus bad":"pumpstatus off";
}

function remainText(v){
  if(v < 0) return "--";
  return v + " sec";
}

async function load(){
  try{
    let r=await fetch("/data?t="+Date.now());
    let d=await r.json();

    document.getElementById("temp").innerHTML=d.showTemp.toFixed(1)+"&deg;C";
    document.getElementById("ph").innerHTML=d.showPH.toFixed(1);
    document.getElementById("turb").innerHTML=d.showTurbidity;
    document.getElementById("level").innerHTML=d.showWaterLevel.toFixed(1)+" cm";

    document.getElementById("realTemp").innerHTML="Real: "+d.realTemp.toFixed(1)+"&deg;C";
    document.getElementById("realPH").innerHTML="Real: "+d.realPH.toFixed(1);
    document.getElementById("realTurb").innerHTML="Real: "+d.realTurbidity;
    document.getElementById("realLevel").innerHTML="Real: "+d.realWaterLevel.toFixed(1)+" cm";

    if(d.showTemp < d.tempLowLimit || d.showTemp > d.tempHighLimit) setBadge("tempBadge","ALERT","bad");
    else setBadge("tempBadge","SAFE","ok");

    if(d.showPH < d.phLowLimit) setBadge("phBadge","ACIDIC","bad");
    else if(d.showPH > d.phHighLimit) setBadge("phBadge","BASIC","bad");
    else setBadge("phBadge","SAFE","ok");

    if(d.showTurbidity > d.turbidityLimit) setBadge("turbBadge","DIRTY","warn");
    else setBadge("turbBadge","OK","ok");

    if(d.waterLevelStatus == "OVERFLOW") setBadge("levelBadge","OVERFLOW","warn");
    else if(d.waterLevelStatus == "UNDERFLOW") setBadge("levelBadge","UNDERFLOW","bad");
    else if(d.waterLevelStatus == "NO READING") setBadge("levelBadge","NO READING","off");
    else setBadge("levelBadge","SAFE","ok");

    setPump("acidStatus",d.acidPump);
    setPump("baseStatus",d.basePump);
    setPump("waterStatus",d.waterPump);
    setPump("oxygenStatus",d.oxygenPump);

    document.getElementById("modeTop").innerHTML=d.demoMode ? "Mode: MANUAL DEMO" : "Mode: REAL SENSOR";
    document.getElementById("modeBadge").innerHTML=d.demoMode ? "MANUAL DEMO MODE" : "REAL SENSOR MODE";

    document.getElementById("waterTop").innerHTML="Water: " + d.waterLevelStatus;
    document.getElementById("waterModeBadge").innerHTML=d.waterAutoMode ? "AUTO MODE" : "MANUAL MODE";
    document.getElementById("waterModeBadge").className=d.waterAutoMode ? "badge bluebadge" : "badge off";

    document.getElementById("waterLevelStatusBadge").innerHTML=d.waterLevelStatus;
    if(d.waterLevelStatus=="OVERFLOW") document.getElementById("waterLevelStatusBadge").className="badge warn";
    else if(d.waterLevelStatus=="UNDERFLOW") document.getElementById("waterLevelStatusBadge").className="badge bad";
    else document.getElementById("waterLevelStatusBadge").className="badge ok";

    document.getElementById("feedStatus").innerHTML=d.feedStatus;
    document.getElementById("feed1Remain").innerHTML=remainText(d.feed1Remain);
    document.getElementById("feed2Remain").innerHTML=remainText(d.feed2Remain);
    document.getElementById("homeSwitch").innerHTML=d.servoHomePressed ? "PRESSED" : "OPEN";
    document.getElementById("targetSwitch").innerHTML=d.servoTargetPressed ? "PRESSED" : "OPEN";

  }catch(e){}
}

setInterval(load,2000);
load();
</script>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

// ================= JSON DATA =================

void handleData() {
  long feed1Remain = remainingSeconds(feed1Target, feed1Armed, feed1Done);
  long feed2Remain = remainingSeconds(feed2Target, feed2Armed, feed2Done);
  updateServoLimitSwitches();

  String json = "{";

  json += "\"demoMode\":" + boolText(demoMode) + ",";

  json += "\"realTemp\":" + String(realTemp, 2) + ",";
  json += "\"realPH\":" + String(realPH, 2) + ",";
  json += "\"realTurbidity\":" + String(realTurbidity) + ",";
  json += "\"turbidityRaw\":" + String(turbidityRaw) + ",";
  json += "\"pondTurbidityBaseline\":" + String(pondTurbidityBaseline) + ",";
  json += "\"realWaterLevel\":" + String(realWaterLevel, 2) + ",";

  json += "\"showTemp\":" + String(showTemp, 2) + ",";
  json += "\"showPH\":" + String(showPH, 2) + ",";
  json += "\"showTurbidity\":" + String(showTurbidity) + ",";
  json += "\"showWaterLevel\":" + String(showWaterLevel, 2) + ",";

  json += "\"tempLowLimit\":" + String(tempLowLimit, 2) + ",";
  json += "\"tempHighLimit\":" + String(tempHighLimit, 2) + ",";
  json += "\"phLowLimit\":" + String(phLowLimit, 2) + ",";
  json += "\"phHighLimit\":" + String(phHighLimit, 2) + ",";
  json += "\"turbidityLimit\":" + String(turbidityLimit) + ",";
  json += "\"waterLevelLimit\":" + String(waterLevelLimit, 2) + ",";
  json += "\"waterSafeDistance\":" + String(waterSafeDistance, 2) + ",";
  json += "\"waterUnderflowDistance\":" + String(waterUnderflowDistance, 2) + ",";

  json += "\"waterLevelStatus\":\"" + waterLevelStatus + "\",";
  json += "\"waterAutoMode\":" + boolText(waterAutoMode) + ",";

  json += "\"servoStopValue\":" + String(servoStopValue) + ",";
  json += "\"servoSpeedValue\":" + String(servoSpeedValue) + ",";
  json += "\"servoRunTimeMs\":" + String(servoRunTimeMs) + ",";
  json += "\"servoPulseActive\":" + boolText(servoPulseActive) + ",";

  json += "\"feedStatus\":\"" + getFeedStatusText() + "\",";
  json += "\"feed1Remain\":" + String(feed1Remain) + ",";
  json += "\"feed2Remain\":" + String(feed2Remain) + ",";
  json += "\"servoHomePressed\":" + boolText(servoHomePressed) + ",";
  json += "\"servoTargetPressed\":" + boolText(servoTargetPressed) + ",";

  json += "\"acidPump\":" + boolText(acidPumpStatus) + ",";
  json += "\"basePump\":" + boolText(basePumpStatus) + ",";
  json += "\"waterPump\":" + boolText(waterPumpStatus) + ",";
  json += "\"oxygenPump\":" + boolText(oxygenPumpStatus);

  json += "}";

  server.send(200, "application/json", json);
}

// ================= WEB HANDLERS =================

void handleSetDemo() {
  if (server.hasArg("t")) demoTemp = server.arg("t").toFloat();
  if (server.hasArg("ph")) demoPH = server.arg("ph").toFloat();
  if (server.hasArg("tu")) demoTurbidity = server.arg("tu").toInt();
  if (server.hasArg("wl")) demoWaterLevel = server.arg("wl").toFloat();

  demoMode = true;

  showTemp = demoTemp;
  showPH = demoPH;
  showTurbidity = demoTurbidity;
  showWaterLevel = demoWaterLevel;

  updateWaterLevelStatus();
  runDemoAutomation();

  server.send(200, "text/plain", "Demo values set and automation started");
}

void handleMode() {
  if (server.hasArg("demo")) {
    int m = server.arg("demo").toInt();
    demoMode = (m == 1);
  }

  readSensors();
  updateWaterLevelStatus();
  server.send(200, "text/plain", demoMode ? "Demo mode" : "Real mode");
}

void handleSetRange() {
  if (server.hasArg("tl")) tempLowLimit = server.arg("tl").toFloat();
  if (server.hasArg("th")) tempHighLimit = server.arg("th").toFloat();

  if (server.hasArg("pl")) phLowLimit = server.arg("pl").toFloat();
  if (server.hasArg("ph")) phHighLimit = server.arg("ph").toFloat();

  if (server.hasArg("tu")) turbidityLimit = server.arg("tu").toInt();

  if (server.hasArg("ws")) waterSafeDistance = server.arg("ws").toFloat();
  if (server.hasArg("wu")) waterUnderflowDistance = server.arg("wu").toFloat();

  if (waterSafeDistance < 1.0) waterSafeDistance = 14.0;
  if (waterUnderflowDistance <= waterSafeDistance) waterUnderflowDistance = waterSafeDistance + 2.0;

  waterLevelLimit = waterUnderflowDistance;

  updateWaterLevelStatus();

  Serial.println("===== RANGE UPDATED =====");
  Serial.print("Temp Range: ");
  Serial.print(tempLowLimit);
  Serial.print(" - ");
  Serial.println(tempHighLimit);

  Serial.print("pH Range: ");
  Serial.print(phLowLimit);
  Serial.print(" - ");
  Serial.println(phHighLimit);

  Serial.print("Turbidity Limit: ");
  Serial.println(turbidityLimit);

  Serial.print("Overflow Below Distance: ");
  Serial.println(waterSafeDistance);

  Serial.print("Underflow Above Distance: ");
  Serial.println(waterUnderflowDistance);

  server.send(200, "text/plain", "Range updated");
}

void handleSetFeed() {
  bool anySet = false;

  if (server.hasArg("f1")) {
    int f1 = server.arg("f1").toInt();

    if (f1 > 0) {
      feed1Target = millis() + (unsigned long)f1 * 60000UL;
      feed1Armed = true;
      feed1Done = false;
      anySet = true;

      Serial.print("Feed 1 set after minutes: ");
      Serial.println(f1);
    }
  }

  if (server.hasArg("f2")) {
    int f2 = server.arg("f2").toInt();

    if (f2 > 0) {
      feed2Target = millis() + (unsigned long)f2 * 60000UL;
      feed2Armed = true;
      feed2Done = false;
      anySet = true;

      Serial.print("Feed 2 set after minutes: ");
      Serial.println(f2);
    }
  }

  if (anySet) {
    server.send(200, "text/plain", "Feed schedule set");
  }
  else {
    server.send(200, "text/plain", "No valid feed time");
  }
}

void handleClearFeed() {
  feed1Armed = false;
  feed2Armed = false;
  feed1Done = false;
  feed2Done = false;
  feedCyclePending = false;
  feedCycleState = FEED_IDLE;
  servoStop360();

  server.send(200, "text/plain", "Feed timers cleared");
}

void handleFeedNow() {
  startFeedingCycle();
  server.send(200, "text/plain", "Feeding started");
}

// ================= PUMP HANDLERS =================

void handleAllOff() {
  allPumpsOff();
  server.send(200, "text/plain", "All OFF");
}

void handleAcidOn() {
  relayOn(RELAY_ACID_PUMP);
  acidPumpStatus = true;
  acidPulseActive = false;
  server.send(200, "text/plain", "Acid ON");
}

void handleAcidOff() {
  relayOff(RELAY_ACID_PUMP);
  acidPumpStatus = false;
  acidPulseActive = false;
  server.send(200, "text/plain", "Acid OFF");
}

void handleAcidPulse() {
  startAcidPulse();
  server.send(200, "text/plain", "Acid pulse");
}

void handleBaseOn() {
  relayOn(RELAY_BASE_PUMP);
  basePumpStatus = true;
  basePulseActive = false;
  server.send(200, "text/plain", "Base ON");
}

void handleBaseOff() {
  relayOff(RELAY_BASE_PUMP);
  basePumpStatus = false;
  basePulseActive = false;
  server.send(200, "text/plain", "Base OFF");
}

void handleBasePulse() {
  startBasePulse();
  server.send(200, "text/plain", "Base pulse");
}

void handleWaterAuto() {
  waterAutoMode = true;
  updateWaterLevelStatus();
  updateWaterPumpAuto();
  server.send(200, "text/plain", "Water auto mode");
}

void handleWaterManual() {
  waterAutoMode = false;
  server.send(200, "text/plain", "Water manual mode");
}

void handleWaterOn() {
  waterAutoMode = false;
  relayOn(RELAY_WATER_PUMP);
  waterPumpStatus = true;
  waterPulseActive = false;
  server.send(200, "text/plain", "Manual Water ON");
}

void handleWaterOff() {
  waterAutoMode = false;
  relayOff(RELAY_WATER_PUMP);
  waterPumpStatus = false;
  waterPulseActive = false;
  server.send(200, "text/plain", "Manual Water OFF");
}

void handleWaterPulse() {
  waterAutoMode = false;
  startWaterPulse();
  server.send(200, "text/plain", "Water pulse");
}

void handleOxygenOn() {
  relayOn(RELAY_OXYGEN);
  oxygenPumpStatus = true;
  oxygenPulseActive = false;
  server.send(200, "text/plain", "Oxygen ON");
}

void handleOxygenOff() {
  relayOff(RELAY_OXYGEN);
  oxygenPumpStatus = false;
  oxygenPulseActive = false;
  server.send(200, "text/plain", "Oxygen OFF");
}

void handleOxygenPulse() {
  startOxygenPulse();
  server.send(200, "text/plain", "Oxygen pulse");
}

// ================= SERVO HANDLERS =================

void handleServo() {
  if (server.hasArg("s")) {
    servoSpeedValue = server.arg("s").toInt();
  }

  if (server.hasArg("time")) {
    servoRunTimeMs = server.arg("time").toInt();
  }

  if (server.hasArg("stop")) {
    servoStopValue = server.arg("stop").toInt();
  }

  if (server.hasArg("d")) {
    int d = server.arg("d").toInt();
    servoDirectionValue = (d >= 0) ? 1 : -1;
  }

  if (servoStopValue < 70) servoStopValue = 70;
  if (servoStopValue > 110) servoStopValue = 110;

  startServoPulse(servoSpeedValue, servoDirectionValue, servoRunTimeMs);

  server.send(200, "text/plain", "Servo running");
}

void handleServoStop() {
  feedCycleState = FEED_IDLE;
  feedCyclePending = false;
  servoStop360();
  server.send(200, "text/plain", "Servo stopped");
}

// ================= STABLE WIFI START =================

void startStableWiFi() {
  WiFi.disconnect(true);
  delay(300);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  IPAddress local_ip(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.softAPConfig(local_ip, gateway, subnet);

  bool ok = WiFi.softAP(AP_NAME, AP_PASS, 6, 0, 4);

  delay(500);

  Serial.println("==================================");
  Serial.print("WiFi AP Status: ");
  Serial.println(ok ? "Started" : "Failed");
  Serial.print("WiFi Name: ");
  Serial.println(AP_NAME);
  Serial.print("Password: ");
  Serial.println(AP_PASS);
  Serial.print("IP Address: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("WiFi Sleep: OFF");
  Serial.println("==================================");
}

// ================= SETUP =================

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(21, 22);

  pHWire.begin(32, 33);

  Serial.println("ESP32 pH Master Started");

  lcd.init();
  lcd.backlight();

  tempSensor.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(SERVO_HOME_SWITCH_PIN, INPUT_PULLUP);
  pinMode(SERVO_TARGET_SWITCH_PIN, INPUT_PULLUP);
  updateServoLimitSwitches();

  pinMode(PH_PIN, INPUT);
  pinMode(TURBIDITY_PIN, INPUT);

  analogSetPinAttenuation(PH_PIN, ADC_11db);
  analogSetPinAttenuation(TURBIDITY_PIN, ADC_11db);

  pinMode(RELAY_ACID_PUMP, OUTPUT);
  pinMode(RELAY_BASE_PUMP, OUTPUT);
  pinMode(RELAY_WATER_PUMP, OUTPUT);
  pinMode(RELAY_OXYGEN, OUTPUT);

  digitalWrite(RELAY_ACID_PUMP, LOW);
  digitalWrite(RELAY_BASE_PUMP, LOW);
  digitalWrite(RELAY_WATER_PUMP, LOW);
  digitalWrite(RELAY_OXYGEN, LOW);

  myServo.attach(SERVO_PIN);
  delay(100);
  myServo.write(servoStopValue);
  delay(300);
  myServo.detach();

  noTone(BUZZER_PIN);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("BlueCycle");
  lcd.setCursor(0, 1);
  lcd.print("Starting WiFi");

  startStableWiFi();

  calibrateTurbidityPondWater();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/setdemo", handleSetDemo);
  server.on("/mode", handleMode);
  server.on("/setrange", handleSetRange);

  server.on("/setfeed", handleSetFeed);
  server.on("/clearfeed", handleClearFeed);
  server.on("/feednow", handleFeedNow);

  server.on("/alloff", handleAllOff);

  server.on("/acidon", handleAcidOn);
  server.on("/acidoff", handleAcidOff);
  server.on("/acidpulse", handleAcidPulse);

  server.on("/baseon", handleBaseOn);
  server.on("/baseoff", handleBaseOff);
  server.on("/basepulse", handleBasePulse);

  server.on("/waterauto", handleWaterAuto);
  server.on("/watermanual", handleWaterManual);
  server.on("/wateron", handleWaterOn);
  server.on("/wateroff", handleWaterOff);
  server.on("/waterpulse", handleWaterPulse);

  server.on("/oxygenon", handleOxygenOn);
  server.on("/oxygenoff", handleOxygenOff);
  server.on("/oxygenpulse", handleOxygenPulse);

  server.on("/servo", handleServo);
  server.on("/servostop", handleServoStop);

  server.begin();

  readSensors();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Ready");
  lcd.setCursor(0, 1);
  lcd.print("192.168.4.1");

  Serial.println("BlueCycle Dashboard Ready");
  Serial.println("Open browser: 192.168.4.1");
  Serial.println("Relay: Active HIGH");
  Serial.println("IN1 GPIO25 = Acid Pump");
  Serial.println("IN2 GPIO26 = Oxygen Pump");
  Serial.println("IN3 GPIO27 = Base Pump");
  Serial.println("IN4 GPIO14 = Water Pump");
  Serial.println("Servo schedule + water auto/manual added");
  Serial.println("Servo HOME switch GPIO16, TARGET switch GPIO17");
}

// ================= LOOP =================

void loop() {
  server.handleClient();

  unsigned long now = millis();

  updatePulses();
  updateServoPulse();
  updateFeedingCycle();

  if (now - lastSensorRead >= 1000) {
    lastSensorRead = now;
    readSensors();
    runRealAutomation();

    if (demoMode) {
      updateWaterLevelStatus();
      updateWaterPumpAuto();
    }
  }

  if (now - lastLCD >= 3000) {
    lastLCD = now;
    updateLCD();
  }
}