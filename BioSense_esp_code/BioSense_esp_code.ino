/*
  BioSense - ESP32 Wearable Health Monitoring System

  Functions:
  - MLX90614 temperature sensor
  - SEN11574 / pulse analog sensor
  - GSR analog stress sensor
  - OLED SSD1306 display
  - BLE communication with mobile app
  - OTA Wi-Fi firmware update
  - Alert LED
  - Push button measurement trigger

  Required libraries:
  - Adafruit MLX90614
  - Adafruit SSD1306
  - Adafruit GFX
  - ESP32 BLE Arduino
  - ArduinoOTA
*/

#include <Wire.h>
#include <WiFi.h>
#include <ArduinoOTA.h>

#include <Adafruit_MLX90614.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ======================================================
// PIN CONFIGURATION
// ======================================================

#define PULSE_PIN       34
#define GSR_PIN         35
#define ALERT_LED_PIN   25
#define BUTTON_PIN      33

#define OLED_SDA        21
#define OLED_SCL        22

// ======================================================
// OLED CONFIGURATION
// ======================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ======================================================
// SENSORS
// ======================================================

Adafruit_MLX90614 mlx = Adafruit_MLX90614();

// ======================================================
// WIFI + OTA CONFIGURATION
// ======================================================

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

#define OTA_HOSTNAME "BioSense-ESP32"
#define OTA_PASSWORD "biosense123"

// ======================================================
// BLE CONFIGURATION
// ======================================================

#define DEVICE_NAME "BioSense"

#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define DATA_CHAR_UUID      "abcd1234-1234-1234-1234-abcdef123456"
#define COMMAND_CHAR_UUID   "dcba4321-4321-4321-4321-fedcba654321"

BLECharacteristic *dataCharacteristic;
BLECharacteristic *commandCharacteristic;

bool deviceConnected = false;
bool measurementEnabled = true;

// ======================================================
// MEASUREMENT VARIABLES
// ======================================================

float bodyTemperature = 0.0;
int bpm = 0;
int gsrRaw = 0;
float stressIndex = 0.0;

String stressLevel = "Normal";
String healthStatus = "Normal";

unsigned long lastMeasurementTime = 0;
const unsigned long measurementInterval = 3000;

// ======================================================
// THRESHOLDS
// ======================================================

const float TEMP_ALERT_THRESHOLD = 38.0;
const int BPM_ALERT_THRESHOLD = 100;
const float STRESS_ALERT_THRESHOLD = 1.50;

float gsrBaseline = 1500.0;

// ======================================================
// BLE SERVER CALLBACKS
// ======================================================

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) {
    deviceConnected = true;
    Serial.println("BLE device connected");
  }

  void onDisconnect(BLEServer *server) {
    deviceConnected = false;
    Serial.println("BLE device disconnected");
    BLEDevice::startAdvertising();
  }
};

// ======================================================
// BLE COMMAND CALLBACKS
// ======================================================

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    std::string value = characteristic->getValue();

    if (value.length() == 0) {
      return;
    }

    String command = String(value.c_str());
    command.trim();

    Serial.print("BLE Command received: ");
    Serial.println(command);

    if (command == "START") {
      measurementEnabled = true;
    } 
    else if (command == "STOP") {
      measurementEnabled = false;
    } 
    else if (command == "MEASURE") {
      performMeasurement();
    } 
    else if (command == "CALIBRATE_GSR") {
      calibrateGSR();
    }
  }
};

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(ALERT_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(ALERT_LED_PIN, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);

  initOLED();
  showStartupScreen();

  initTemperatureSensor();
  initBLE();
  initOTA();

  Serial.println("BioSense system ready.");
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop() {
  ArduinoOTA.handle();

  handleButton();

  if (measurementEnabled && millis() - lastMeasurementTime >= measurementInterval) {
    lastMeasurementTime = millis();
    performMeasurement();
  }

  delay(20);
}

// ======================================================
// INITIALIZATION FUNCTIONS
// ======================================================

void initOLED() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED initialization failed");
    while (true) {
      delay(1000);
    }
  }

  display.clearDisplay();
  display.display();
}

void initTemperatureSensor() {
  if (!mlx.begin()) {
    Serial.println("MLX90614 not detected");

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 20);
    display.println("MLX90614 ERROR");
    display.display();

    while (true) {
      ArduinoOTA.handle();
      delay(100);
    }
  }

  Serial.println("MLX90614 ready");
}

void initBLE() {
  BLEDevice::init(DEVICE_NAME);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(SERVICE_UUID);

  dataCharacteristic = service->createCharacteristic(
    DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  dataCharacteristic->addDescriptor(new BLE2902());

  commandCharacteristic = service->createCharacteristic(
    COMMAND_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );

  commandCharacteristic->setCallbacks(new CommandCallbacks());

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);

  BLEDevice::startAdvertising();

  Serial.println("BLE advertising started");
}

void initOTA() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi for OTA");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    ArduinoOTA.handle();
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println();
    Serial.println("Wi-Fi connection failed. OTA disabled.");
    return;
  }

  Serial.println();
  Serial.println("Wi-Fi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA update started");

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 20);
    display.println("OTA Update...");
    display.display();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("OTA update finished");

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 20);
    display.println("OTA Done");
    display.display();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int percent = (progress * 100) / total;

    Serial.printf("OTA Progress: %u%%\r", percent);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 20);
    display.print("OTA Progress: ");
    display.print(percent);
    display.println("%");
    display.display();
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]\n", error);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 20);
    display.println("OTA Error");
    display.display();
  });

  ArduinoOTA.begin();

  Serial.println("OTA ready");
}

// ======================================================
// MAIN MEASUREMENT FUNCTION
// ======================================================

void performMeasurement() {
  readAllSensors();
  processHealthStatus();
  updateOLED();
  sendBLEData();
  printSerialData();
}

// ======================================================
// SENSOR READING FUNCTIONS
// ======================================================

void readAllSensors() {
  bodyTemperature = readTemperature();
  bpm = readHeartRate();
  gsrRaw = readGSR();
  stressIndex = calculateStressIndex(gsrRaw);
  stressLevel = classifyStress(stressIndex);
}

float readTemperature() {
  float temp = mlx.readObjectTempC();

  if (isnan(temp)) {
    return 0.0;
  }

  return temp;
}

int readGSR() {
  const int samples = 20;
  long sum = 0;

  for (int i = 0; i < samples; i++) {
    ArduinoOTA.handle();
    sum += analogRead(GSR_PIN);
    delay(5);
  }

  return sum / samples;
}

float calculateStressIndex(int value) {
  if (gsrBaseline <= 0) {
    return 0.0;
  }

  return (float)value / gsrBaseline;
}

String classifyStress(float index) {
  if (index < 1.15) {
    return "Low";
  } 
  else if (index < 1.50) {
    return "Moderate";
  } 
  else {
    return "High";
  }
}

// ======================================================
// HEART RATE PROCESSING
// ======================================================

int readHeartRate() {
  const unsigned long durationMs = 5000;
  const int threshold = 2200;
  const int hysteresis = 200;

  int beats = 0;
  bool pulseDetected = false;

  unsigned long startTime = millis();

  while (millis() - startTime < durationMs) {
    ArduinoOTA.handle();

    int signal = analogRead(PULSE_PIN);

    if (signal > threshold && !pulseDetected) {
      beats++;
      pulseDetected = true;
    }

    if (signal < threshold - hysteresis) {
      pulseDetected = false;
    }

    delay(10);
  }

  int calculatedBPM = beats * 12;

  if (calculatedBPM < 40 || calculatedBPM > 180) {
    calculatedBPM = 0;
  }

  return calculatedBPM;
}

// ======================================================
// HEALTH STATUS
// ======================================================

void processHealthStatus() {
  bool alert = false;

  if (bodyTemperature > TEMP_ALERT_THRESHOLD) {
    alert = true;
  }

  if (bpm > BPM_ALERT_THRESHOLD && bpm != 0) {
    alert = true;
  }

  if (stressIndex > STRESS_ALERT_THRESHOLD) {
    alert = true;
  }

  if (alert) {
    healthStatus = "Alert";
    digitalWrite(ALERT_LED_PIN, HIGH);
  } 
  else {
    healthStatus = "Normal";
    digitalWrite(ALERT_LED_PIN, LOW);
  }
}

// ======================================================
// OLED DISPLAY FUNCTIONS
// ======================================================

void updateOLED() {
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("BioSense Monitor");

  display.setCursor(0, 14);
  display.print("Temp: ");
  display.print(bodyTemperature, 1);
  display.println(" C");

  display.setCursor(0, 26);
  display.print("BPM : ");
  display.println(bpm);

  display.setCursor(0, 38);
  display.print("GSR : ");
  display.println(gsrRaw);

  display.setCursor(0, 50);
  display.print("Stress: ");
  display.println(stressLevel);

  if (healthStatus == "Alert") {
    display.setCursor(88, 0);
    display.println("ALERT");
  }

  display.display();
}

void showStartupScreen() {
  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(10, 15);
  display.println("BioSense");

  display.setTextSize(1);
  display.setCursor(10, 45);
  display.println("BLE + OTA Monitor");

  display.display();
  delay(2000);
}

// ======================================================
// BLE COMMUNICATION
// ======================================================

void sendBLEData() {
  String jsonData = "{";
  jsonData += "\"temperature\":";
  jsonData += String(bodyTemperature, 1);
  jsonData += ",";

  jsonData += "\"bpm\":";
  jsonData += String(bpm);
  jsonData += ",";

  jsonData += "\"gsr\":";
  jsonData += String(gsrRaw);
  jsonData += ",";

  jsonData += "\"stressIndex\":";
  jsonData += String(stressIndex, 2);
  jsonData += ",";

  jsonData += "\"stressLevel\":\"";
  jsonData += stressLevel;
  jsonData += "\",";

  jsonData += "\"status\":\"";
  jsonData += healthStatus;
  jsonData += "\"";

  jsonData += "}";

  dataCharacteristic->setValue(jsonData.c_str());

  if (deviceConnected) {
    dataCharacteristic->notify();
  }
}

// ======================================================
// BUTTON HANDLING
// ======================================================

void handleButton() {
  static bool lastButtonState = HIGH;
  static unsigned long lastDebounceTime = 0;

  bool currentButtonState = digitalRead(BUTTON_PIN);

  if (currentButtonState != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > 50) {
    if (lastButtonState == HIGH && currentButtonState == LOW) {
      Serial.println("Button pressed: manual measurement");
      performMeasurement();
    }
  }

  lastButtonState = currentButtonState;
}

// ======================================================
// GSR CALIBRATION
// ======================================================

void calibrateGSR() {
  long sum = 0;
  const int samples = 100;

  Serial.println("Calibrating GSR...");

  for (int i = 0; i < samples; i++) {
    ArduinoOTA.handle();
    sum += analogRead(GSR_PIN);
    delay(10);
  }

  gsrBaseline = sum / samples;

  Serial.print("New GSR baseline: ");
  Serial.println(gsrBaseline);
}

// ======================================================
// SERIAL DEBUG
// ======================================================

void printSerialData() {
  Serial.println("========== BioSense Data ==========");

  Serial.print("Temperature: ");
  Serial.print(bodyTemperature);
  Serial.println(" C");

  Serial.print("BPM: ");
  Serial.println(bpm);

  Serial.print("GSR Raw: ");
  Serial.println(gsrRaw);

  Serial.print("Stress Index: ");
  Serial.println(stressIndex);

  Serial.print("Stress Level: ");
  Serial.println(stressLevel);

  Serial.print("Status: ");
  Serial.println(healthStatus);

  Serial.print("BLE Connected: ");
  Serial.println(deviceConnected ? "YES" : "NO");

  Serial.print("Wi-Fi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("OTA IP: ");
    Serial.println(WiFi.localIP());
  }

  Serial.println("===================================");
}