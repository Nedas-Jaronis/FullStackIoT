// MCU-A: Freenove ESP32-WROVER — Vision + Network
// Board: "ESP32 Wrover Module" in Arduino IDE
// Flash: just plug in USB-C, no FTDI needed
// Waits for capture trigger from MCU-B via ESP-NOW
// Captures JPEG → POSTs to gateway → sends result back to MCU-B via ESP-NOW

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include "esp_camera.h"
#include "esp_task_wdt.h"

// ── Config ───────────────────────────────────────────────────────────────────
#include "secrets.h"

const char* GATEWAY_URL = "https://glasstint-gateway-478053964713.us-central1.run.app/verify";

// MCU-B's MAC address — fill in once teammate sends it
// Leave as FF:FF:FF:FF:FF:FF to run in SOLO TEST MODE (no MCU-B needed)
uint8_t MCU_B_MAC[6] = {0x6C, 0xC8, 0x40, 0x76, 0x54, 0x74};

// ── Solo test mode ────────────────────────────────────────────────────────────
// When MCU_B_MAC is all FF (not set), type anything in Serial Monitor to trigger
// a capture. Result prints to Serial only — no ESP-NOW send.
// Once you fill in MCU-B's MAC above, solo mode disables automatically.

// ── Freenove ESP32-WROVER camera pin config ──────────────────────────────────
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     21
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       19
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM        5
#define Y2_GPIO_NUM        4
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ── Shared ESP-NOW message structs — must match MCU-B exactly ───────────────
// Trigger: MCU-B → MCU-A
typedef struct {
  char cmd[8];  // "CAPTURE"
} TriggerMsg;

// Result: MCU-A → MCU-B
typedef struct {
  bool trusted;
  char name[32];
  char description[64];
  int  confidence_pct;  // 0–100
} ResultMsg;

// ── State ────────────────────────────────────────────────────────────────────
volatile bool captureRequested = false;
esp_now_peer_info_t peerInfo;

bool soloMode() {
  // Solo mode = MCU-B MAC not set yet
  for (int i = 0; i < 6; i++) if (MCU_B_MAC[i] != 0xFF) return false;
  return true;
}

// ── ESP-NOW callbacks ────────────────────────────────────────────────────────
void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len != sizeof(TriggerMsg)) return;
  TriggerMsg msg;
  memcpy(&msg, data, sizeof(msg));
  if (strcmp(msg.cmd, "CAPTURE") == 0) {
    captureRequested = true;
  }
}

void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
  Serial.printf("ESP-NOW send: %s\n",
    status == ESP_NOW_SEND_SUCCESS ? "ok" : "failed");
}

// ── Camera init ──────────────────────────────────────────────────────────────
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
  cfg.xclk_freq_hz  = 10000000;
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
  s->set_framesize(s, FRAMESIZE_QVGA);  // 320x240 for fast POST to gateway

  // Discard first frame — sensor needs one cycle to adjust exposure
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
  delay(100);
  return true;
}

// ── Wi-Fi + ESP-NOW init ──────────────────────────────────────────────────────
void initWiFiAndESPNOW() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // credentials from secrets.h
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("MCU-A MAC: %s  <-- give this to MCU-B sketch\n",
                WiFi.macAddress().c_str());
  Serial.printf("WiFi channel: %d\n", WiFi.channel());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed — check Wi-Fi mode is WIFI_STA");
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
    Serial.println("Failed to add MCU-B as peer — check MAC address");
  }
}

// ── POST image to gateway ────────────────────────────────────────────────────
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
  client.setInsecure();  // skip TLS cert verification — fine for embedded demo
  HTTPClient http;
  http.begin(client, GATEWAY_URL);
  http.addHeader("X-Device-Id", "esp32-cam");
  http.setTimeout(60000);  // 60s — Cloud Run cold start can take 30-60s

  // Build multipart/form-data body manually (no library needed)
  String boundary  = "GlassTintBound";
  String partHead  = "--" + boundary + "\r\n"
                     "Content-Disposition: form-data; name=\"image\"; filename=\"cap.jpg\"\r\n"
                     "Content-Type: image/jpeg\r\n\r\n";
  String partTail  = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = partHead.length() + fb->len + partTail.length();
  uint8_t* body   = (uint8_t*)malloc(totalLen);
  if (!body) {
    Serial.println("malloc failed — not enough heap");
    esp_camera_fb_return(fb);
    sendResult(false, "error", "", 0);
    return;
  }

  memcpy(body,                                  partHead.c_str(), partHead.length());
  memcpy(body + partHead.length(),              fb->buf,          fb->len);
  memcpy(body + partHead.length() + fb->len,   partTail.c_str(), partTail.length());
  esp_camera_fb_return(fb);  // release frame buffer ASAP

  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  int code = http.POST(body, totalLen);
  free(body);

  if (code == 200) {
    String resp = http.getString();
    Serial.println("Gateway response: " + resp);
    http.end();
    delay(500);  // let WiFi radio settle before ESP-NOW
    if (soloMode()) {
      Serial.println(">>> SOLO MODE: result above is what MCU-B would receive <<<");
    } else {
      parseAndForward(resp);
    }
  } else {
    Serial.printf("HTTP error: %d\n", code);
    http.end();
    if (!soloMode()) sendResult(false, "error", "", 0);
  }
}

// ── Minimal JSON field extraction (no ArduinoJson needed) ───────────────────
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

// ── Send result to MCU-B via ESP-NOW ─────────────────────────────────────────
void sendResult(bool trusted, const char* name, const char* desc, int conf_pct) {
  ResultMsg msg = {};
  msg.trusted        = trusted;
  msg.confidence_pct = conf_pct;
  strncpy(msg.name,        name, sizeof(msg.name) - 1);
  strncpy(msg.description, desc, sizeof(msg.description) - 1);

  delay(1000);  // let radio settle after HTTP
  esp_err_t res = esp_now_send(MCU_B_MAC, (uint8_t*)&msg, sizeof(msg));
  Serial.printf("sendResult → trusted=%d name=%s conf=%d%% (%s)\n",
    msg.trusted, msg.name, msg.confidence_pct,
    res == ESP_OK ? "queued" : "send error");
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
  delay(3000);
  Serial.begin(115200);
  Serial.println("\n=== MCU-A: GlassTint Vision Node ===");

  esp_task_wdt_deinit();  // disable watchdog — camera init can be slow

  if (!initCamera()) {
    Serial.println("FATAL: camera failed. Check ribbon cable and power cycle.");
    while (true) delay(1000);
  }
  Serial.println("Camera OK");

  initWiFiAndESPNOW();

  if (soloMode()) {
    Serial.println("*** SOLO TEST MODE — MCU-B MAC not set ***");
    Serial.println("Type anything in Serial Monitor and press Enter to trigger a capture.");
    Serial.println("Results print here only. ESP-NOW send is skipped.");
  } else {
    Serial.println("Ready — waiting for CAPTURE trigger from MCU-B via ESP-NOW...");
  }
}

void loop() {
  // ESP-NOW trigger (from MCU-B when paired)
  if (captureRequested) {
    captureRequested = false;
    captureAndVerify();
    delay(500);
    return;
  }

  // Solo test mode: any Serial input triggers capture
  if (soloMode() && Serial.available()) {
    while (Serial.available()) Serial.read();  // flush input
    captureAndVerify();
  }

  delay(10);
}
