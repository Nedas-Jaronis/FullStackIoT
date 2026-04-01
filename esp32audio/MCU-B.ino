#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <math.h>

// WiFi config 
const char* ssid = "";
const char* password = "";

// Gateway server
const char* gatewayUrl = "http://IP of laptop:5000";

// Pin assignments
#define BUTTON_PIN    13
#define GREEN_LED     2    // match led
#define RED_LED       4    // unknown led
#define BLUE_LED      15   // processing led

// I2S config for Audio Converter and Amplifier
#define I2S_BCK       26
#define I2S_LCK       25
#define I2S_DIN       22
#define SAMPLE_RATE   16000

// ESP-NOW
#include <esp_now.h>
// MAC address of MCU-A (ESP32-CAM) | TODO: Update this after flashing MCU-A
uint8_t mcuA_address[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// message structure shared between MCU-A and MCU-B
typedef struct {
  char command[16];   // "capture" or "result"
  char payload[64];   // name result from gateway
} esp_msg_t;

esp_msg_t outgoing;
esp_msg_t incoming;
volatile bool resultReady = false;

// I2S setup
void setupI2S() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCK,
    .ws_io_num = I2S_LCK,
    .data_out_num = I2S_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
}

// flush silence to stop any leftover noise from the speaker
void silenceI2S() {
  int16_t silence[2] = {0, 0};
  for (int i = 0; i < 1000; i++) {
    size_t written;
    i2s_write(I2S_NUM_0, silence, sizeof(silence), &written, portMAX_DELAY);
  }
}

// Play a tone through the speaker
void playTone(int frequency, int duration_ms) {
  int samples = (SAMPLE_RATE * duration_ms) / 1000;
  int16_t sample[2]; // stereo: left + right

  for (int i = 0; i < samples; i++) {
    float t = (float)i / SAMPLE_RATE;
    int16_t value = (int16_t)(sin(2.0 * M_PI * frequency * t) * 16000);
    sample[0] = value; // left channel
    sample[1] = value; // right channel
    size_t written;
    i2s_write(I2S_NUM_0, sample, sizeof(sample), &written, portMAX_DELAY);
  }
  silenceI2S();
}

// match sound, two rising tones
void playMatchSound() {
  playTone(800, 150);
  playTone(1200, 200);
}

// unknown sound, single low tone
void playUnknownSound() {
  playTone(300, 400);
}

// processing sound, quick beep
void playProcessingBeep() {
  playTone(600, 100);
}

//  LED helpers
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

// ESP-NOW callbacks
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  memcpy(&incoming, data, sizeof(incoming));
  if (strcmp(incoming.command, "result") == 0) {
    resultReady = true;
  }
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "ok" : "fail");
}

// Setup
void setup() {
  Serial.begin(115200);

  // pins
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  ledsOff();

  // I2S audio
  setupI2S();

  // WiFi Setup
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP: ");
  Serial.println(WiFi.localIP());

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
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  // startup indicator
  playProcessingBeep();
  Serial.println("MCU-B ready");
}

// Main loop
void loop() {
  // wait for button press (active LOW because of pull-up)
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50); // debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("Button pressed — sending capture command to MCU-A");

      showProcessing();
      playProcessingBeep();

      // tell MCU-A to capture a photo
      strcpy(outgoing.command, "capture");
      strcpy(outgoing.payload, "");
      esp_now_send(mcuA_address, (uint8_t *)&outgoing, sizeof(outgoing));

      // wait for result from MCU-A (timeout after 15 seconds)
      unsigned long start = millis();
      resultReady = false;
      while (!resultReady && (millis() - start < 15000)) {
        delay(100);
      }

      if (resultReady) {
        String name = String(incoming.payload);
        Serial.print("Result: ");
        Serial.println(name);

        if (name == "unknown") {
          showUnknown();
          playUnknownSound();
        } else {
          showMatch();
          playMatchSound();
          // TODO: replace tones with TTS audio for the actual name
        }
      } else {
        Serial.println("Timeout — no response from MCU-A");
        showUnknown();
        playUnknownSound();
      }

      // keep LED on for 3 seconds then turn off
      delay(3000);
      ledsOff();

      // wait for button release
      while (digitalRead(BUTTON_PIN) == LOW) {
        delay(50);
      }
    }
  }
}
