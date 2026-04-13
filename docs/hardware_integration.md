# Hardware Integration Guide

This document describes how to connect the ESP32-CAM and audio ESP32 to the existing gateway pipeline.

The gateway (`gateway/app.py`) is already hardware-ready. The `/verify` endpoint accepts a JPEG image POST from any HTTP client and returns JSON — the ESP32s just need to send/receive that. No changes to the Python backend are needed.

## This repo with hardware connected

The web UI doesn't get replaced by the hardware — it becomes the **management dashboard** for the glasses:

| UI Feature | Role with hardware running |
|---|---|
| **History tab** | Live feed of every scan the glasses perform. Events show `device_id: esp32-cam` so hardware scans are distinguishable from web UI tests. |
| **Admin panel** | How you enroll new trusted people without touching code or SQL. Upload photos → instantly recognized by the glasses. |
| **Verify tab** | Still used for testing new enrollments before a demo, or when hardware isn't available. |
| **Health dot** | Confirms the gateway is up and accepting scans from the ESP32-CAM. |

The full system architecture is:

```
[ESP32-CAM glasses]  →  POST /verify  →  [Laptop gateway]  →  [Supabase cloud]
                                               ↑                      ↑
                                         Web UI (same laptop)   DB + Storage
                                      (dashboard + admin)     (events + photos)
```

This is a complete Level 4 IoT architecture: IoT device → edge compute → cloud storage → management dashboard.

---

## How the pipeline works end-to-end

```
[Button press on glasses]
        ↓
[ESP32-CAM captures JPEG]
        ↓
[POST /verify  →  gateway laptop  →  DeepFace ArcFace]
        ↓
[JSON response: name + trusted + description + confidence]
        ↓
[Audio ESP32 reads response → speaks result via speaker]
```

---

## Track 2 — ESP32-CAM (`esp32cam/capture_and_send.ino`)

### What it needs to do
1. Wait for button press
2. Capture a JPEG from the OV2640 camera
3. POST it to `http://<LAPTOP_IP>:5000/verify` with header `X-Device-Id: esp32-cam`
4. Pass the raw JSON response body to the audio ESP32 over Serial (or Wi-Fi)

### Key sketch structure

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"

const char* SSID       = "YOUR_WIFI";
const char* PASSWORD   = "YOUR_PASSWORD";
const char* GATEWAY_URL = "http://192.168.X.X:5000/verify";  // laptop's LAN IP
const int   BUTTON_PIN  = 12;

void setup() {
  Serial.begin(115200);
  // Camera init (AI Thinker pin config — copy standard config for this board)
  camera_config_t config;
  // ... standard AI Thinker pin assignments ...
  esp_camera_init(&config);

  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void loop() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);  // debounce
    captureAndVerify();
    delay(500);
  }
}

void captureAndVerify() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return;

  HTTPClient http;
  http.begin(GATEWAY_URL);
  http.addHeader("X-Device-Id", "esp32-cam");

  // Multipart form upload — field name must be "image"
  String boundary = "----GlassTintBoundary";
  String bodyStart = "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"image\"; filename=\"capture.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
  String bodyEnd = "\r\n--" + boundary + "--\r\n";

  int totalLen = bodyStart.length() + fb->len + bodyEnd.length();
  uint8_t* body = (uint8_t*)malloc(totalLen);
  memcpy(body, bodyStart.c_str(), bodyStart.length());
  memcpy(body + bodyStart.length(), fb->buf, fb->len);
  memcpy(body + bodyStart.length() + fb->len, bodyEnd.c_str(), bodyEnd.length());

  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  int httpCode = http.POST(body, totalLen);
  free(body);
  esp_camera_fb_return(fb);

  if (httpCode == 200) {
    String response = http.getString();
    // Forward JSON to audio ESP32 over hardware Serial2
    Serial2.println(response);
  }
  http.end();
}
```

### Wiring (AI Thinker ESP32-CAM)

| Pin | Connection |
|-----|-----------|
| GPIO 12 | Button → GND (internal pull-up) |
| GPIO 4 | Built-in flash LED (optional feedback) |
| Serial2 TX (GPIO 17) | → Audio ESP32 RX |
| GND | Common ground with audio ESP32 |

### Important notes
- ESP32-CAM has no USB-serial chip. Flash using an FTDI adapter wired to the UART0 pins. Hold IO0 LOW during upload, then release.
- The laptop running the gateway must be on the same Wi-Fi network. Find its IP with `ipconfig` (Windows) or `ifconfig`/`ip a` (Mac/Linux). The IP must be static or reserved via router DHCP, otherwise it changes.
- Make sure uvicorn is started with `--host 0.0.0.0` (not `127.0.0.1`) so it accepts connections from the network.

---

## Track 3 — Audio ESP32 (`esp32audio/speak_result.ino`)

### What it needs to do
1. Listen on Serial2 for a JSON string from the ESP32-CAM
2. Parse `trusted`, `result`, and `description` from the JSON
3. Speak the result using a TTS library over the DAC/I2S output

### Key sketch structure

```cpp
#include <Arduino.h>
#include <ArduinoJson.h>
// Use ESP8266Audio or ESP32-audioI2S for TTS/audio output
// For simple spoken output, a pre-recorded audio lookup or
// a text-to-speech service called over Wi-Fi works well.

const int SPEAKER_PIN = 25;  // DAC output

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17);  // RX=16, TX=17
  // Audio/DAC init here
}

void loop() {
  if (Serial2.available()) {
    String json = Serial2.readStringUntil('\n');
    handleResult(json);
  }
}

void handleResult(String json) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return;

  bool trusted       = doc["trusted"];
  const char* name   = doc["result"];
  const char* desc   = doc["description"] | "";  // null-safe

  // Build spoken string: "Trusted. Nedas. Family." or "Unknown person detected."
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

  speakText(speech);
}

void speakText(String text) {
  // Option A: ESP32 built-in DAC + pre-recorded audio clips mapped to names
  // Option B: I2S DAC + ESP32-audioI2S library streaming TTS from an online API
  // Option C: Simple UART to a DFPlayer Mini MP3 module with pre-recorded clips
  Serial.println("Speaking: " + text);
  // ... audio output implementation here ...
}
```

### TTS options (pick one)

| Option | Complexity | Cost | Notes |
|--------|-----------|------|-------|
| **DFPlayer Mini** | Low | ~$2 | Pre-record MP3s for each name + "trusted"/"unknown", play by index. No internet needed. |
| **ESP32-audioI2S + online TTS** | Medium | Free | Stream audio from Google TTS or similar API over Wi-Fi to I2S DAC. Requires internet. |
| **Espressif ESP-TTS** | Medium | Free | On-device TTS, limited vocabulary, no internet needed. |

For this project, **DFPlayer Mini** is the simplest path: record short clips for each enrolled person's name + description, wire the DFPlayer to Serial2, and map the JSON result to the right clip index.

### Wiring (ESP32 Dev Module)

| ESP32 Pin | Connection |
|-----------|-----------|
| GPIO 16 (RX2) | ESP32-CAM TX (Serial2) |
| GPIO 17 (TX2) | ESP32-CAM RX (Serial2) |
| GPIO 25 (DAC1) | Amplifier input (if using DAC) |
| GPIO 26 (TX to DFPlayer) | DFPlayer RX (if using DFPlayer) |
| GND | Common ground |

---

## Connecting to the existing pipeline — checklist

- [ ] Laptop is on Wi-Fi, uvicorn running with `--host 0.0.0.0 --port 5000`
- [ ] Laptop IP is fixed (set static IP or DHCP reservation in router)
- [ ] `GATEWAY_URL` in ESP32-CAM sketch points to correct laptop IP
- [ ] ESP32-CAM and laptop on same Wi-Fi network (2.4 GHz)
- [ ] ESP32-CAM serial TX wired to audio ESP32 RX (common GND)
- [ ] Audio ESP32 parses JSON and maps `result` + `description` to speech output
- [ ] Test end-to-end: press button → LED flashes → `POST /verify` appears in server logs → audio speaks result

## Testing without the glasses assembled

You can test each piece independently:

```bash
# Simulate an ESP32-CAM POST from your laptop
curl -X POST \
  -H "X-Device-Id: esp32-cam" \
  -F "image=@enrolled/Anthony/Anthony1.jpg" \
  http://localhost:5000/verify
```

Expected response:
```json
{
  "result": "Anthony",
  "trusted": true,
  "description": "Brother",
  "confidence": 0.82,
  "latency_ms": 3200
}
```

Then check the History tab in the web UI — the event should appear with `device_id: esp32-cam`.
