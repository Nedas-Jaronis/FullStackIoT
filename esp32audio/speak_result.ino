#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Audio.h"

// WiFi credentials
const char* SSID     = "YOUR_WIFI_SSID";
const char* PASSWORD = "YOUR_WIFI_PASSWORD";

// I2S pins for speaker output
// if using the DAC directly (no I2S amp), use the following:
# define I2S_DOUT  25   // DAC output to amplifier
# define I2S_BCLK  26
# define I2S_LRC   27

Audio audio;

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17); // RX=16, TX=17 from ESP32-CAM

  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(15); // goes from 0 – 21 for volume output
}

void loop() {
  // check for JSON result from ESP32-CAM over Serial2
  if (Serial2.available()) {
    String json = Serial2.readStringUntil('\n');
    json.trim();
    if (json.length() > 0) {
      handleResult(json);
    }
  }
  audio.loop(); // keep audio streaming
}

void handleResult(String json) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.println("JSON parse error");
    return;
  }

  bool trusted       = doc["trusted"]     | false;
  const char* name   = doc["result"]      | "unknown";
  const char* desc   = doc["description"] | "";

  // build spoken string
  String speech;
  if (trusted) {
    speech = "Trusted. ";
    speech += name;
    speech += ".";
    if (strlen(desc) > 0) {
      speech += " ";
      speech += desc;
      speech += ".";
    }
  } else {
    speech = "Unknown person detected.";
  }

  Serial.println("Speaking: " + speech);
  speakText(speech);
}

void speakText(String text) {
  // url-encode the text for Google talk-to-speech
  String encoded = "";
  for (int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == ' ') {
      encoded += "+";
    } else if (isAlphaNumeric(c) || c == '.' || c == '-') {
      encoded += c;
    } else {
      encoded += "%" + String((int)c, HEX);
    }
  }

  // Google talk-to-speech url
  String url = "http://translate.google.com/translate_tts?ie=UTF-8&tl=en&client=tw-ob&q=" + encoded;

  Serial.println("TTS URL: " + url);
  audio.connecttohost(url.c_str());
}

// required audio callbacks (can leave empty)
void audio_info(const char *info)         { Serial.println(info); }
void audio_eof_mp3(const char *info)      { Serial.println("Done speaking"); }