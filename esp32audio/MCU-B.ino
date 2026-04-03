#include <WiFi.h>
#include <esp_now.h>
#include "Audio.h"

// WiFi config from secrets.h (needed for Google TTS streaming)
#include "secrets.h"

// Pin assignments
#define BUTTON_PIN    13
#define GREEN_LED     2    // match led
#define RED_LED       4    // unknown led
#define BLUE_LED      15   // processing led

// I2S pins for PCM5102A
#define I2S_BCK       26
#define I2S_LCK       25
#define I2S_DIN       22

// Audio library handles I2S for us
Audio audio;

// ESP-NOW
// MAC address of MCU-A — update after flashing MCU-A
uint8_t mcuA_address[] = {0x6C, 0xC8, 0x40, 0x78, 0xFE, 0x40};

// message structs — MUST match MCU-A exactly
typedef struct {
  char cmd[8];  // "CAPTURE"
} TriggerMsg;

typedef struct {
  bool trusted;
  char name[32];
  char description[64];
  int  confidence_pct;  // 0-100
} ResultMsg;

TriggerMsg triggerOut;
ResultMsg  resultIn;
volatile bool resultReady = false;
volatile bool listening = false; // only accept results when we're waiting for one
bool audioAllowed = false; // only run audio.loop() when we want audio

// LED helpers
void ledsOff() {
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BLUE_LED, LOW);
}

void showProcessing() {
  ledsOff();
  digitalWrite(BLUE_LED, HIGH);
}

void showMatch() {
  ledsOff();
  digitalWrite(GREEN_LED, HIGH);
}

void showUnknown() {
  ledsOff();
  digitalWrite(RED_LED, HIGH);
}

// TTS — speak text through Google Translate
void speakText(String text) {
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

  String url = "https://translate.google.com/translate_tts?ie=UTF-8&tl=en&client=tw-ob&q=" + encoded;
  Serial.println("Speaking: " + text);
  audioAllowed = true;
  audio.connecttohost(url.c_str());

  // wait for first complete playthrough then kill connection
  unsigned long ttsStart = millis();
  bool started = false;
  while (millis() - ttsStart < 10000) {
    audio.loop();
    if (audio.isRunning()) {
      started = true;
    } else if (started) {
      break;
    }
    delay(1);
  }
  // fully kill audio so it can't replay
  audio.stopSong();
  audioAllowed = false;
  audio.setPinout(I2S_BCK, I2S_LCK, I2S_DIN);
  delay(300);
}

// ESP-NOW callbacks (v2.x API)
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (listening && !resultReady && len == sizeof(ResultMsg)) {
    memcpy(&resultIn, data, sizeof(resultIn));
    resultReady = true;
    listening = false; // got one result, ignore everything else
  }
}

void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "ok" : "fail");
}

void setup() {
  Serial.begin(115200);

  // pins
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  ledsOff();

  // WiFi (needed for TTS streaming and ESP-NOW)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("WiFi channel: %d\n", WiFi.channel());

  // Audio setup
  audio.setPinout(I2S_BCK, I2S_LCK, I2S_DIN);
  audio.setVolume(8); // 0-21

  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  // register MCU-A as peer
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, mcuA_address, 6);
  peer.channel = WiFi.channel(); // must match MCU-A's WiFi channel
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.print("MCU-B MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("MCU-B ready");
}

void loop() {
  if (audioAllowed) audio.loop(); // only process audio when we want it

  // wait for button press
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50); // debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("Button pressed — sending CAPTURE to MCU-A");

      // clear any stale results and start listening for one new result
      resultReady = false;
      listening = true;
      showProcessing();

      // send CAPTURE trigger to MCU-A
      strcpy(triggerOut.cmd, "CAPTURE");
      esp_now_send(mcuA_address, (uint8_t *)&triggerOut, sizeof(triggerOut));

      // wait for result from MCU-A (timeout 90s, hold button 2s to cancel)
      unsigned long start = millis();
      unsigned long holdStart = 0;
      while (!resultReady && (millis() - start < 90000)) {
        if (audioAllowed) audio.loop();
        // hold button for 2 seconds to cancel
        if (digitalRead(BUTTON_PIN) == LOW) {
          if (holdStart == 0) holdStart = millis();
          if (millis() - holdStart > 2000) {
            Serial.println("Cancelled by button hold");
            break;
          }
        } else {
          holdStart = 0;
        }
        delay(50);
      }

      if (resultReady) {
        // grab result and immediately clear flag so retries are ignored
        bool trusted = resultIn.trusted;
        String name = String(resultIn.name);
        int conf = resultIn.confidence_pct;
        resultReady = false;

        Serial.printf("Result: trusted=%d name=%s conf=%d%%\n", trusted, name.c_str(), conf);

        if (trusted) {
          showMatch();
          speakText("Trusted. " + name);
        } else {
          showUnknown();
          speakText("Unknown person");
        }
      } else {
        Serial.println("Timeout — no response from MCU-A");
        showUnknown();
      }

      // stop any lingering audio and drain retries
      audio.stopSong();
      resultReady = false;
      delay(500);
      resultReady = false;

      // keep LED on for 2 seconds then turn off
      delay(2000);
      ledsOff();

      // wait for button release
      while (digitalRead(BUTTON_PIN) == LOW) {
        delay(50);
      }

      // final clear — ready for next press
      resultReady = false;
    }
  }
}

// Audio library callbacks
void audio_info(const char *info) { Serial.println(info); }
void audio_eof_mp3(const char *info) { Serial.println("Done speaking"); }
