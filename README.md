# GlassTint

IoT-enabled assistive glasses for trusted person recognition. Helps visually impaired and memory-impaired users identify nearby people through wearable face verification with audio feedback.

**Architecture:** ESP32-CAM captures a face on button press, sends it to a laptop gateway running DeepFace verification, and an audio ESP32 speaks the result. Cloud logging via Supabase.

## Prerequisites

- Python 3.9–3.11 (3.12+ may have compatibility issues with some dependencies)
- pip
- Git
- Arduino IDE 2.x (for ESP32 firmware)
- A webcam or ESP32-CAM module
- Supabase account (free tier) for cloud logging

## Gateway Setup (Track 1 — Laptop)

This is the critical path. Get this running first.

### 1. Clone the repo

```bash
git clone https://github.com/YOUR_USERNAME/FullStackIoT.git
cd FullStackIoT
```

### 2. Create a virtual environment

```bash
python -m venv venv

# Windows
venv\Scripts\activate

# macOS/Linux
source venv/bin/activate
```

### 3. Install Python dependencies

```bash
pip install fastapi uvicorn python-multipart deepface opencv-python supabase python-dotenv
```

**What each package does:**

| Package | Purpose |
|---------|---------|
| `fastapi` | Async HTTP server that receives images from ESP32-CAM and returns results |
| `uvicorn` | ASGI server to run FastAPI |
| `python-multipart` | Required for file upload handling in FastAPI |
| `deepface` | Face detection, embedding extraction, and verification (wraps FaceNet, ArcFace, etc.) |
| `opencv-python` | Image capture and preprocessing |
| `supabase` | Cloud logging client for event metadata |
| `python-dotenv` | Loads environment variables from `.env` file |

DeepFace will automatically download model weights on first run (~100–500 MB depending on the model). Default model is VGG-Face; you can switch to ArcFace or Facenet for better accuracy:

```python
DeepFace.verify(img1, img2, model_name="ArcFace")
```

### 4. Enroll trusted faces

Create an `enrolled/` directory with subdirectories for each person. Add 3–5 clear photos per person.

```
enrolled/
  Anthony/
    photo1.jpg
    photo2.jpg
    photo3.jpg
  Nedas/
    photo1.jpg
    photo2.jpg
```

Photos should be well-lit, front-facing, and taken at roughly the distance the camera will be used.

### 5. Configure environment variables

Create a `.env` file in the project root:

```env
PORT=5000
CONFIDENCE_THRESHOLD=0.6
SUPABASE_URL=https://your-project.supabase.co
SUPABASE_KEY=your-anon-key
```

### 6. Run the gateway server

```bash
uvicorn gateway.app:app --host 0.0.0.0 --port 5000
```

The server starts on `http://0.0.0.0:5000`. Interactive API docs are auto-generated at `http://localhost:5000/docs`. Test it:

```bash
# Test with a local image
curl -X POST -F "image=@test_photo.jpg" http://localhost:5000/verify
```

Expected response:

```json
{"result": "Anthony", "confidence": 0.82}
```

or

```json
{"result": "unknown", "confidence": 0.0}
```

## ESP32-CAM Setup (Track 2 — MCU-A)

### 1. Configure and flash

1. Open `esp32cam/capture_and_send.ino` in Arduino IDE
2. Update Wi-Fi credentials and gateway IP in the sketch:
   ```cpp
   const char* ssid = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";
   const char* serverUrl = "http://LAPTOP_IP:5000/verify";
   ```
3. Select board: **Tools > Board > AI Thinker ESP32-CAM**
4. Select port and upload

### 3. Wiring

- **Button:** GPIO 12 to GND (internal pull-up used)
- **LED indicator:** GPIO 4 (built-in flash LED)

## Audio MCU Setup (Track 3 — MCU-B)

### 1. Flash the second ESP32

1. Open `esp32audio/speak_result.ino` in Arduino IDE
2. Update Wi-Fi credentials
3. Select board: **Tools > Board > ESP32 Dev Module**
4. Upload

### 2. Wiring

- **Speaker + amplifier:** DAC output (GPIO 25) to amplifier input
- **Amplifier output:** to speaker terminals
- **Button (optional):** GPIO 12 to GND

## Supabase Setup (Track 4 — Cloud Logging)

### 1. Create a Supabase project

1. Go to [supabase.com](https://supabase.com) and create a free account
2. Create a new project

### 2. Get your credentials

1. Go to **Settings > API** in your Supabase dashboard
2. Copy the **Project URL** and **anon public** key
3. Add them to your `.env` file

## Project Structure

```
FullStackIoT/
  gateway/
    app.py              # FastAPI server with DeepFace verification
  enrolled/
    [name]/             # Enrolled face photos per person
  esp32cam/
    capture_and_send.ino  # ESP32-CAM firmware
  esp32audio/
    speak_result.ino      # Audio ESP32 firmware
  .env                  # Environment config (not committed)
  requirements.txt      # Python dependencies
```

## Quick Test (No Hardware)

You can test the full gateway pipeline with just your laptop:

```bash
# 1. Install dependencies
pip install fastapi uvicorn python-multipart deepface opencv-python

# 2. Add some face photos to enrolled/YourName/
# 3. Start the server
uvicorn gateway.app:app --host 0.0.0.0 --port 5000

# 4. Test with curl or Postman
curl -X POST -F "image=@path/to/test_photo.jpg" http://localhost:5000/verify
```

## Troubleshooting

| Issue | Fix |
|-------|-----|
| `ModuleNotFoundError: No module named 'cv2'` | Run `pip install opencv-python` |
| DeepFace model download hangs | Check internet connection; models are ~100–500 MB |
| ESP32-CAM won't connect to Wi-Fi | Verify SSID/password; ensure 2.4 GHz network (not 5 GHz) |
| `ConnectionRefused` from ESP32 | Check laptop firewall; ensure uvicorn binds to `0.0.0.0` not `127.0.0.1` |
| Low confidence scores | Add more enrolled photos with varied lighting/angles |
| `CAMERA_INIT_FAILED` on ESP32-CAM | Check camera ribbon cable connection; try power cycling |

## Hardware BOM

| Part | Purpose | Cost |
|------|---------|------|
| ESP32-CAM (from kit) | Image capture MCU | $0 |
| ESP32 Dev Board (from kit) | Audio/control MCU | $0 |
| Speaker + amplifier (from kit) | Audio feedback | $0 |
| Tactile buttons (from kit) | User input | $0 |
| Glasses frame | Wearable mount | $10 |
| LiPo battery | Portable power | $10 |
| USB-C charge module | Battery charging | $6 |
| 3D print material | Camera mount | $5 |
| Enclosure accessories | Protection | $7 |
| **Total** | | **$38** |

## Team

- Nedas Jaronis — jaronisnedas@ufl.edu
- Anthony D. Madorsky — anthonymadorsky@ufl.edu
- Natania Philippe — nphilippe@ufl.edu
- Hemdutt Rao — raohemdutt@ufl.edu

University of Florida

## License

MIT
