#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <NTC_Thermistor.h>
#include <AverageThermistor.h>

#include "WiFi.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// WiFi
#define WIFI_NETWORK "Registered4OSU"
#define WIFI_PASSWORD ""
#define WIFI_TIMEOUT_MS 10000

// n8n webhook
const char* WEBHOOK_URL = "";

// =====================
// LCD
// =====================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// =====================
// Stop list
// =====================
const char* stops[] = {
  "High St 15th",
  "Stillman",
  "Ohio Union SB",
  "Siebert",
  "Mack Hall",
  "Herrick Hub",
  "11th & Worth",
  "Ohio Union NB",
  "Arps Hall",
  "Blackburn",
  "Mason Hall",
  "St John Arena",
  "Midwest WB",
  "Fred Taylor"
};

const int stopCount = sizeof(stops) / sizeof(stops[0]);
int currentStopIndex = 0;

// =====================
// Pins
// =====================
#define SDA_PIN 21
#define SCL_PIN 22
#define PHOTORESISTOR_PIN 34
#define BUTTON 35
#define BUTTON2 32
#define LED_PIN 2
#define SPEAKER_PIN 25

// =====================
// Button tracking
// =====================
int lastActionButtonState = LOW;   // GPIO 35: external button, HIGH when pressed
int lastScrollButtonState = HIGH;  // GPIO 32: INPUT_PULLUP, LOW when pressed

unsigned long lastActionPressTime = 0;
unsigned long lastScrollPressTime = 0;
const unsigned long debounceDelay = 250;

// =====================
// Helpers
// =====================

void printLCDLine(int row, String text) {
  if (text.length() > 16) {
    text = text.substring(0, 16);
  }

  while (text.length() < 16) {
    text += " ";
  }

  lcd.setCursor(0, row);
  lcd.print(text);
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NETWORK, WIFI_PASSWORD);

  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_TIMEOUT_MS) {
    Serial.print(".");
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" Failed!");
  } else {
    Serial.print(" Connected! IP: ");
    Serial.println(WiFi.localIP());
  }
}

String escapeJsonString(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", " ");
  s.replace("\r", " ");
  return s;
}

void playToneOnce(int freq, int durationMs) {
  ledcWriteTone(SPEAKER_PIN, freq);
  delay(durationMs);
  ledcWriteTone(SPEAKER_PIN, 0);
}

void playScrollBeep() {
  playToneOnce(1000, 100);
}

void playTakeCurrentBusTone() {
  playToneOnce(1400, 140);
}

void playWaitNextBusTone() {
  playToneOnce(950, 120);
  delay(80);
  playToneOnce(950, 120);
}

void playWalkToDifferentStopTone() {
  playToneOnce(700, 100);
  delay(60);
  playToneOnce(900, 100);
  delay(60);
  playToneOnce(1100, 100);
}

void playUseTripShotTone() {
  playToneOnce(500, 350);
}

void playGenericDecisionTone() {
  playToneOnce(1100, 100);
  delay(60);
  playToneOnce(1300, 100);
}

void playDecisionToneFromText(String text) {
  String lower = text;
  lower.toLowerCase();

  if (lower.indexOf("trip") != -1 || lower.indexOf("unsafe") != -1) {
    playUseTripShotTone();
  } else if (lower.indexOf("walk") != -1) {
    playWalkToDifferentStopTone();
  } else if (lower.indexOf("wait") != -1 || lower.indexOf("next") != -1) {
    playWaitNextBusTone();
  } else if (lower.indexOf("take") != -1 || lower.indexOf("current") != -1) {
    playTakeCurrentBusTone();
  } else {
    playGenericDecisionTone();
  }
}

// =====================
// Main webhook logic
// =====================

void callN8NWebhook(int brightness, bool dark, int buttonState, const char* selectedStop) {
  if (WiFi.status() != WL_CONNECTED) {
    printLCDLine(1, "No WiFi");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, WEBHOOK_URL)) {
    printLCDLine(1, "Hook failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(15000);
  http.setTimeout(30000);

  String json = "{";
  json += "\"selected_stop\":\"" + escapeJsonString(String(selectedStop)) + "\",";
  json += "\"origin_stop\":\"Fontana Lab\",";
  json += "\"brightness\":" + String(brightness) + ",";
  json += "\"dark\":" + String(dark ? "true" : "false") + ",";
  json += "\"action_button\":" + String(buttonState) + ",";
  json += "\"device\":\"fontana_lab_stop\",";
  json += "\"decision_goal\":\"Decide whether the rider should take the current bus, wait for the next bus, walk to a different stop, or use TripShot if it is genuinely unsafe. Prioritize quickness by default. Darkness alone is not automatically unsafe. Dark plus bad weather should push more strongly toward TripShot.\"";
  json += "}";

  int httpCode = http.POST(json);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("n8n response:");
    Serial.println(response);

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
      printLCDLine(1, "Bad JSON");
      Serial.println(error.c_str());
      http.end();
      return;
    }

    String text = doc["text"] | "No text";

    text.replace("\n", " ");
    text.replace("\r", " ");

    if (text.length() > 16) {
      text = text.substring(0, 16);
    }

    printLCDLine(1, text);
    playDecisionToneFromText(text);

  } else {
    printLCDLine(1, "HTTP error");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
}

// =====================
// Setup / Loop
// =====================

void setup() {
  Serial.begin(9600);
  delay(2000);

  Serial.println();
  Serial.println("Starting setup...");

  WiFi.mode(WIFI_STA);
  delay(100);

  Serial.print("ESP32 WiFi MAC: ");
  Serial.println(WiFi.macAddress());

  connectToWiFi();

  pinMode(PHOTORESISTOR_PIN, INPUT);
  pinMode(BUTTON, INPUT);
  pinMode(BUTTON2, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  // Tone output on speaker pin
  ledcAttach(SPEAKER_PIN, 2000, 8);

  Wire.begin(SDA_PIN, SCL_PIN);

  lcd.init();
  lcd.backlight();

  // Show first stop on boot
  printLCDLine(0, stops[currentStopIndex]);
  printLCDLine(1, "");
}

void loop() {
  int brightness = analogRead(PHOTORESISTOR_PIN);
  int buttonState = digitalRead(BUTTON);    // GPIO 35, HIGH when pressed
  int buttonState2 = digitalRead(BUTTON2);  // GPIO 32, LOW when pressed
  bool dark = brightness < 2000;

  // LED behavior from photoresistor
  if (dark) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
  }

  // Keep selected stop on first LCD line
  printLCDLine(0, stops[currentStopIndex]);

  // Button 1: action button, trigger webhook for current stop
  if (buttonState == HIGH && lastActionButtonState == LOW) {
    if (millis() - lastActionPressTime > debounceDelay) {
      Serial.println("Action button pressed");
      callN8NWebhook(brightness, dark, buttonState, stops[currentStopIndex]);
      lastActionPressTime = millis();
    }
  }

  // Button 2: scroll button, advance only on HIGH -> LOW transition
  if (buttonState2 == LOW && lastScrollButtonState == HIGH) {
    if (millis() - lastScrollPressTime > debounceDelay) {
      currentStopIndex++;
      if (currentStopIndex >= stopCount) {
        currentStopIndex = 0;
      }

      printLCDLine(0, stops[currentStopIndex]);
      Serial.print("Scrolled to stop: ");
      Serial.println(stops[currentStopIndex]);

      lastScrollPressTime = millis();
      playScrollBeep();
    }
  }

  lastActionButtonState = buttonState;
  lastScrollButtonState = buttonState2;

  delay(5);
}