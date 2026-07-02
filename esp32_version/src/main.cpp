#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include "SHT21.h"

// ==================== SETTINGS ====================
// 1. Wi-Fi credentials
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// 2. Google Apps Script Web App URL (MUST end with a question mark)
const String serverName = "https://script.google.com/macros/s/[-=YOUR_Google_APi_code=-]/exec?";

// 3. Logging interval (10 minutes) expressed in microseconds
#define TIME_TO_SLEEP  600000000 
// ==================================================

// Custom I2C pinout for your wiring
#define MY_SDA 41
#define MY_SCL 42
#define Vext 36  // Heltec V3 internal power switch for external rails

SHT21 sht21; // SHT21 sensor object

void setup() {
  // Initialize Serial Monitor for debugging
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- OfficeAir ESP32 (SHT21) Started ---");

  // Heltec V3 power management: enable power to external pins
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW); // LOW turns power ON, HIGH turns power OFF
  delay(100);              // Wait for the sensor to stabilize

  // Initialize I2C bus with custom pins (SDA: 41, SCL: 42)
  Wire.begin(MY_SDA, MY_SCL);
  Serial.println("[Sensor] I2C bus initialized.");

  // Connect to Wi-Fi network
  WiFi.begin(ssid, password);
  Serial.print("[Wi-Fi] Connecting...");
  
  int attempts = 0;
  // Try connecting for ~10 seconds (20 * 500ms)
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[Wi-Fi] Connected successfully!");
    Serial.print("[Wi-Fi] ESP32 IP Address: ");
    Serial.println(WiFi.localIP());
    
    // Read data from SHT21 sensor
    float temp = sht21.getTemperature();
    float hum = sht21.getHumidity();
    
    // Sanity check to ensure we received valid numbers
    if (!isnan(temp) && !isnan(hum) && temp > -40.0) {
      Serial.print("[Sensor] Readings -> Temp: ");
      Serial.print(temp, 1);
      Serial.print(" C, Humidity: ");
      Serial.print(hum, 1);
      Serial.println(" %");

      // === BUILD THE DATA STRING ===
      // Using "temperature" exactly as expected by the Google Sheet headers
      String data = "temperature=" + String(temp, 1) + "&humidity=" + String(hum, 1);
      
      // Combine base URL with data string
      String finalUrl = serverName + data;
      
      // Print full URL to serial for verification
      Serial.print("[HTTP] Sending URL: ");
      Serial.println(finalUrl);
      Serial.println("[HTTP] Publishing to Google Sheets...");
      
      HTTPClient http;
      // Google uses 302 redirects, so enabling redirect-following is mandatory!
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      http.begin(finalUrl);
      
      int httpResponseCode = http.GET();
      
      if (httpResponseCode > 0) {
        Serial.print("[HTTP] Response Code: ");
        Serial.println(httpResponseCode);
        String payload = http.getString();
        Serial.println("[HTTP] Server Response: " + payload);
      } else {
        Serial.print("[HTTP] ERROR! Code: ");
        Serial.println(httpResponseCode);
      }
      http.end(); // Close connection
    } else {
      Serial.println("[Sensor] ERROR! Failed to read data from SHT21.");
    }
  } else {
    Serial.println("\n[Wi-Fi] ERROR! Failed to connect within timeout.");
  }

  // Power management: turn off external power to the sensor before sleeping
  digitalWrite(Vext, HIGH);
  
  // Enter Deep Sleep
  Serial.println("[System] Entering deep sleep for 10 minutes...");
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP);
  esp_deep_sleep_start();
}

void loop() {
  // Loop remains empty as the ESP32 restarts into setup() after deep sleep wakeup
}