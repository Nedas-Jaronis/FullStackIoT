#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include "esp_camera.h"
#include "esp_task_wdt.h"

// WiFi credentials
#include "secrets.h"

const char* GATEWAY_URL = "https://glasstint-gateway-478053964713.us-central1.run.app/verify";

// MAC of MCU-B. Leave as all 0xFF to run standalone (no ESP-NOW forwarding).
uint8_t MCU_B_MAC[6] = {0x6C, 0xC8, 0x40, 0x76, 0x54, 0x74};

// Camera pins for the XIAO ESP32S3 Sense
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    10
#define SIOD_GPIO_NUM    40
#define SIOC_GPIO_NUM    39
#define Y9_GPIO_NUM      48
#define Y8_GPIO_NUM      11
#define Y7_GPIO_NUM      12
#define Y6_GPIO_NUM      14
#define Y5_GPIO_NUM      16
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM      17
#define Y2_GPIO_NUM      15
#define VSYNC_GPIO_NUM   38
#define HREF_GPIO_NUM    47
#define PCLK_GPIO_NUM    13

// These two structs must match MCU-B exactly
typedef struct {
  char cmd[8];  // "CAPTURE"
} TriggerMsg;

typedef struct {
  bool trusted;
  char name[32];
  char description[64];
  int  confidence_pct;  // 0 to 100
} ResultMsg;

volatile bool captureRequested = false;
esp_now_peer_info_t peerInfo;

bool soloMode() {
  // If MCU-B MAC is all 0xFF, skip ESP-NOW sends
  for (int i = 0; i < 6; i++) if (MCU_B_MAC[i] != 0xFF) return false;
  return true;
}

// ESP-NOW callbacks, using ESP32 core v3.x signatures
void onDataRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
  if (len != sizeof(TriggerMsg)) return;
  TriggerMsg msg;
  memcpy(&msg, data, sizeof(msg));
  if (strcmp(msg.cmd, "CAPTURE") == 0) {
    captureRequested = true;
  }
}

void onDataSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  Serial.printf("ESP-NOW send: %s\n",
    status == ESP_NOW_SEND_SUCCESS ? "ok" : "failed");
}

bool initCamera() {
  camera_config_t cfg = {};
  cfg.ledc_channel  = LEDC_CHANNEL_0;
  cfg.ledc_timer    = LEDC_TIMER_0;
  cfg.pin_d0        = Y2_GPIO_NUM;
  cfg.pin_d1        = Y3_GPIO_NUM;
  cfg.pin_d2        = Y4_GPIO_NUM;
  cfg.pin_d3        = Y5_GPIO_NUM;
  cfg.pin_d4        = Y6_GPIO_NUM;
  cfg.pin_d5        = Y7_GPIO_NUM;
  cfg.pin_d6        = Y8_GPIO_NUM;
  cfg.pin_d7        = Y9_GPIO_NUM;
  cfg.pin_xclk      = XCLK_GPIO_NUM;
  cfg.pin_pclk      = PCLK_GPIO_NUM;
  cfg.pin_vsync     = VSYNC_GPIO_NUM;
  cfg.pin_href      = HREF_GPIO_NUM;
  cfg.pin_sccb_sda  = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl  = SIOC_GPIO_NUM;
  cfg.pin_pwdn      = PWDN_GPIO_NUM;
  cfg.pin_reset     = RESET_GPIO_NUM;
  cfg.xclk_freq_hz  = 20000000;
  cfg.pixel_format  = PIXFORMAT_JPEG;
  cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
  cfg.fb_location   = CAMERA_FB_IN_PSRAM;
  cfg.frame_size    = FRAMESIZE_UXGA;
  cfg.jpeg_quality  = 12;
  cfg.fb_count      = 1;

  if (psramFound()) {
    cfg.jpeg_quality = 10;
    cfg.fb_count     = 2;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    cfg.frame_size  = FRAMESIZE_SVGA;
    cfg.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  s->set_framesize(s, FRAMESIZE_QVGA);

  // First frame after boot is usually garbage, throw it away
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
  delay(100);
  return true;
}

void initWiFiAndESPNOW() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("MCU-A MAC: %s  (paste this into MCU-B)\n",
                WiFi.macAddress().c_str());
  Serial.printf("WiFi channel: %d\n", WiFi.channel());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed. Is WiFi mode WIFI_STA?");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, MCU_B_MAC, 6);
  peerInfo.channel = WiFi.channel();
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Could not add MCU-B as a peer. Check the MAC address.");
  }
}

void captureAndVerify() {
  Serial.println("Capturing photo...");

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    sendResult(false, "error", "", 0);
    return;
  }
  Serial.printf("Captured %u bytes. Sending to gateway...\n", fb->len);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, GATEWAY_URL);
  http.addHeader("X-Device-Id", "esp32-cam");
  // Cloud Run can take 30 to 60 seconds on a cold start, so give it time
  http.setTimeout(60000);

  // Build a multipart/form-data body by hand so we don't need another library
  String boundary  = "GlassTintBound";
  String partHead  = "--" + boundary + "\r\n"
                     "Content-Disposition: form-data; name=\"image\"; filename=\"cap.jpg\"\r\n"
                     "Content-Type: image/jpeg\r\n\r\n";
  String partTail  = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = partHead.length() + fb->len + partTail.length();
  uint8_t* body   = (uint8_t*)malloc(totalLen);
  if (!body) {
    Serial.println("malloc failed, not enough heap");
    esp_camera_fb_return(fb);
    sendResult(false, "error", "", 0);
    return;
  }

  memcpy(body,                                  partHead.c_str(), partHead.length());
  memcpy(body + partHead.length(),              fb->buf,          fb->len);
  memcpy(body + partHead.length() + fb->len,   partTail.c_str(), partTail.length());
  esp_camera_fb_return(fb);

  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  int code = http.POST(body, totalLen);
  free(body);

  if (code == 200) {
    String resp = http.getString();
    Serial.println("Gateway response: " + resp);
    http.end();
    delay(500);
    if (soloMode()) {
      Serial.println(">>> SOLO MODE: result above is what MCU-B would have received");
    } else {
      parseAndForward(resp);
    }
  } else {
    Serial.printf("HTTP error: %d\n", code);
    http.end();
    if (!soloMode()) sendResult(false, "error", "", 0);
  }
}

// Tiny JSON helpers so we don't pull in ArduinoJson
String jsonStr(const String& json, const String& key) {
  String needle = "\"" + key + "\":\"";
  int s = json.indexOf(needle);
  if (s < 0) return "";
  s += needle.length();
  int e = json.indexOf('"', s);
  return e < 0 ? "" : json.substring(s, e);
}

bool jsonBool(const String& json, const String& key) {
  String needle = "\"" + key + "\":";
  int p = json.indexOf(needle);
  if (p < 0) return false;
  return json.substring(p + needle.length(), p + needle.length() + 4) == "true";
}

int jsonConfidencePct(const String& json) {
  String needle = "\"confidence\":";
  int p = json.indexOf(needle);
  if (p < 0) return 0;
  return (int)(json.substring(p + needle.length()).toFloat() * 100.0f);
}

void parseAndForward(const String& json) {
  sendResult(
    jsonBool(json, "trusted"),
    jsonStr(json, "result").c_str(),
    jsonStr(json, "description").c_str(),
    jsonConfidencePct(json)
  );
}

void sendResult(bool trusted, const char* name, const char* desc, int conf_pct) {
  ResultMsg msg = {};
  msg.trusted        = trusted;
  msg.confidence_pct = conf_pct;
  strncpy(msg.name,        name, sizeof(msg.name) - 1);
  strncpy(msg.description, desc, sizeof(msg.description) - 1);

  delay(500);

  // The HTTP request tends to disrupt the peer, so re-add it before sending
  esp_now_del_peer(MCU_B_MAC);
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, MCU_B_MAC, 6);
  peerInfo.channel = WiFi.channel();
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  for (int i = 0; i < 3; i++) {
    esp_err_t res = esp_now_send(MCU_B_MAC, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("sendResult attempt %d, trusted=%d name=%s conf=%d%% (%s)\n",
      i + 1, msg.trusted, msg.name, msg.confidence_pct,
      res == ESP_OK ? "queued" : "send error");
    if (res == ESP_OK) break;
    delay(300);
  }
}

void setup() {
  delay(3000);
  Serial.begin(115200);
  Serial.println("\n=== MCU-A: GlassTint Vision Node ===");

  // Camera init can take a while, keep the watchdog out of the way
  esp_task_wdt_deinit();

  if (!initCamera()) {
    Serial.println("FATAL: camera failed. Check the ribbon cable and power cycle.");
    while (true) delay(1000);
  }
  Serial.println("Camera OK");

  initWiFiAndESPNOW();

  if (soloMode()) {
    Serial.println("*** SOLO TEST MODE. MCU-B MAC not set. ***");
    Serial.println("Type anything in Serial Monitor to trigger a capture.");
    Serial.println("Results print here only, no ESP-NOW send.");
  } else {
    Serial.println("Ready. Waiting for CAPTURE trigger from MCU-B.");
  }
}

void loop() {
  if (captureRequested) {
    captureRequested = false;
    captureAndVerify();
    delay(500);
    return;
  }

  // Solo test mode: any Serial input triggers a capture
  if (soloMode() && Serial.available()) {
    while (Serial.available()) Serial.read();
    captureAndVerify();
  }

  delay(10);
}
