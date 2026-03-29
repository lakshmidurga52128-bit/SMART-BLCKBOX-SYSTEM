#include <SoftwareSerial.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <MPU6050.h>

// -------- SERIAL --------
SoftwareSerial gpsSerial(4, 3);
SoftwareSerial gsmSerial(10, 11);
TinyGPSPlus gps;

// -------- PINS --------
#define trigPin 8
#define echoPin 2
#define vibrationPin 6
#define alcoholPin A0
#define buzzerPin 7
#define carPin 5
#define motorPin 12

// -------- LIMITS --------
#define SPEED_LIMIT 80
#define ALCOHOL_LIMIT 350
#define DIST_LIMIT 20
// -------- MPU --------
MPU6050 mpu;
int16_t ax, ay, az;
float axg, ayg, azg;

// -------- FLAGS --------
bool accident = false;
bool accidentSent = false;
bool gpsFault = false;
bool gsmFault = false;
bool accelFault = false;

// -------- DATA --------
float speed = 0;
float latitude = 0;
float longitude = 0;
int alcoholValue = 0;
int distance = 0;

// Pre-accident memory
float lastSpeed = 0;
float lastLat = 0;
float lastLon = 0;

// -------- TIMERS --------
unsigned long lastUpload = 0;

// -------- ISR --------
volatile unsigned long startTime = 0, endTime = 0;
volatile bool measurementComplete = false;

void echoISR() {
  if (digitalRead(echoPin))
    startTime = micros();
  else {
    endTime = micros();
    measurementComplete = true;
  }
}

// -------- SETUP --------
void setup() {
  Serial.begin(9600);
  gpsSerial.begin(9600);
  gsmSerial.begin(9600);

  Wire.begin();
  mpu.initialize();

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(vibrationPin, INPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(carPin, INPUT_PULLUP);
  pinMode(motorPin, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(echoPin), echoISR, CHANGE);

  if (!mpu.testConnection()) accelFault = true;

  Serial.println("SYSTEM READY");
}

// -------- LOOP --------
void loop() {

  readGPS();
  readMPU();
  alcoholValue = analogRead(alcoholPin);
  distance = getDistance();

  bool carON = digitalRead(carPin) == HIGH;

  if (carON) {
    
    Serial.println("CAR STATUS : ON");
    digitalWrite(motorPin, HIGH);

    checkSpeed();
    checkAlcohol();
    checkRashDriving();
    checkAccident();
     
    if (millis() - lastUpload > 20000) {
      Serial.println("UPLOADING TO THINGSPEAK");
      uploadThingSpeak();
      lastUpload = millis();
    }

  } else {
     Serial.println("CAR STATUS : OFF");
    digitalWrite(motorPin, LOW);
    checkTamper();
  }

  displaySerial();
}

// -------- GPS --------
void readGPS() {
  gpsSerial.listen();

  while (gpsSerial.available())
    gps.encode(gpsSerial.read());

  if (gps.location.isValid()) {
    latitude = gps.location.lat();
    longitude = gps.location.lng();
    gpsFault = false;
  } else gpsFault = true;

  if (gps.speed.isValid())
    speed = gps.speed.kmph();

  // Store last values (pre-accident)
  lastSpeed = speed;
  lastLat = latitude;
  lastLon = longitude;
}

// -------- MPU --------
void readMPU() {
  mpu.getAcceleration(&ax, &ay, &az);

  axg = ax / 16384.0;
  ayg = ay / 16384.0;
  azg = az / 16384.0;
}

// -------- DISTANCE --------
int getDistance() {
  measurementComplete = false;

  digitalWrite(trigPin, LOW);
  delayMicroseconds(5);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long timeout = millis();

  while (!measurementComplete) {
    if (millis() - timeout > 50) return 400;
  }

  float dist = (endTime - startTime) * 0.034 / 2;

  if (dist < 2 || dist > 400) return 400;

  return (int)dist;
}

// -------- SPEED --------
void checkSpeed() {
  if (speed > SPEED_LIMIT) {
    digitalWrite(buzzerPin, HIGH);
  } else {
    digitalWrite(buzzerPin, LOW);
  }
}

// -------- ALCOHOL --------
void checkAlcohol() {
  if (alcoholValue > ALCOHOL_LIMIT) {
    Serial.println("ALCOHOL DETECTED");
    digitalWrite(motorPin, LOW);
    sendSMS("DRUNK DRIVER ALERT");
  }
}

// -------- RASH DRIVING --------
void checkRashDriving() {
  if (abs(axg) > 2 || abs(ayg) > 2)
    Serial.println("RASH DRIVING");
}

// -------- ACCIDENT --------
void checkAccident() {

  if (distance < DIST_LIMIT || abs(axg) > 3 || abs(ayg) > 3 || abs(azg) > 3) {
    accident = true;
  }

  if (abs(azg) < 0.2) {
    Serial.println("ROLLOVER DETECTED");
    accident = true;
  }

  if (accident && !accidentSent) {

    Serial.println("ACCIDENT_DETECTED");

    digitalWrite(motorPin, LOW);
    digitalWrite(buzzerPin, HIGH);

    sendSMS("ACCIDENT ALERT");
    makeCall();

    uploadThingSpeak();

    accidentSent = true;
  }
}

// -------- TAMPER --------
void checkTamper() {
  if (digitalRead(vibrationPin) == HIGH) {
    Serial.println("TAMPER_ALERT");
    digitalWrite(buzzerPin, HIGH);
    sendSMS("THEFT ALERT");
    delay(2000);
  }
}

// -------- THINGSPEAK --------
void uploadThingSpeak() {

  gsmSerial.listen();

  gsmSerial.println("AT");
  delay(1000);

  gsmSerial.println("AT+SAPBR=1,1");
  delay(3000);

  gsmSerial.println("AT+HTTPINIT");
  delay(1000);

  gsmSerial.print("AT+HTTPPARA=\"URL\",\"http://api.thingspeak.com/update?api_key=PTDJYH6GKDDE55PM");
  gsmSerial.print("&field1="); gsmSerial.print(speed);
  gsmSerial.print("&field2="); gsmSerial.print(distance);
  gsmSerial.print("&field3="); gsmSerial.print(alcoholValue);
  gsmSerial.print("&field4="); gsmSerial.print(latitude,6);
  gsmSerial.print("&field5="); gsmSerial.print(longitude,6);
  gsmSerial.println("\"");

  delay(2000);
  gsmSerial.println("AT+HTTPACTION=0");
  delay(6000);
}

// -------- SMS --------
void sendSMS(const char* msg) {

  gsmSerial.listen();

  gsmSerial.println("AT+CMGF=1");
  delay(1000);

  gsmSerial.println("AT+CMGS=\"+916385401984\"");
  delay(1000);

  gsmSerial.println(msg);

  gsmSerial.print("Speed: "); gsmSerial.println(lastSpeed);
  gsmSerial.print("Location: https://maps.google.com/?q=");
  gsmSerial.print(lastLat,6); gsmSerial.print(",");
  gsmSerial.println(lastLon,6);

  gsmSerial.write(26);
  delay(5000);
}

// -------- CALL --------
void makeCall() {
  gsmSerial.println("ATD+916385401984;");
  delay(15000);
  gsmSerial.println("ATH");
}

// -------- SERIAL --------
void displaySerial() {
  Serial.print("Speed:"); Serial.print(speed);
  Serial.print(" Dist:"); Serial.print(distance);
  Serial.print(" Alcohol:"); Serial.print(alcoholValue);
  Serial.print(" Lat:"); Serial.print(latitude,6);
  Serial.print(" Lon:"); Serial.print(longitude,6);

  Serial.print(" AX:"); Serial.print(axg);
  Serial.print(" AY:"); Serial.print(ayg);
  Serial.print(" AZ:"); Serial.println(azg);
}