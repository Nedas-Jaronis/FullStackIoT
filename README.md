# GlassTint

IoT-enabled assistive glasses for trusted person recognition. Helps visually impaired and memory-impaired users identify nearby people through wearable face verification with audio feedback.

**Architecture:** MCU-A (Freenove ESP32-WROVER) captures a face on button press and POSTs it to a cloud gateway (Google Cloud Run) running DeepFace ArcFace verification. The result is sent back to MCU-B via ESP-NOW — MCU-B handles the button, LED, audio feedback, and battery. Enrolled people, reference photos, and recognition history are stored in Supabase.

---

## Live Cloud Gateway

The gateway is deployed and running at:

```
https://glasstint-gateway-478053964713.us-central1.run.app
```

You can test it directly without running anything locally:

```bash
# Health check
curl https://glasstint-gateway-478053964713.us-central1.run.app/health

# Face verification
curl -X POST -F "image=@path/to/photo.jpg" \
  https://glasstint-gateway-478053964713.us-central1.run.app/verify
```

The web admin UI is available at the same URL in a browser.

---

## Prerequisites

- Python 3.9–3.11 (3.12+ may have compatibility issues with some dependencies)
- pip
- Git
- Arduino IDE 2.x (for ESP32 firmware)
- A webcam or ESP32-WROVER module
- Supabase account (free tier) for cloud storage and logging

---

## Gateway Setup (Local Development)

If you want to run the gateway locally (e.g. to test code changes before deploying):

### 1. Clone the repo

```bash
git clone https://github.com/YOUR_USERNAME/FullStackIoT.git
cd FullStackIoT
```

### 2. Create a virtual environment

```bash
python -m venv venv

# macOS/Linux
source venv/bin/activate

# Windows
venv\Scripts\activate
```

### 3. Install Python dependencies

```bash
pip install -r requirements.txt
```

**What each package does:**

| Package | Purpose |
|---------|---------|
| `fastapi` | Async HTTP server that receives images from ESP32 and returns results |
| `uvicorn` | ASGI server to run FastAPI |
| `python-multipart` | Required for file upload handling in FastAPI |
| `deepface` | Face detection, embedding extraction, and verification (ArcFace model) |
| `opencv-python-headless` | Image preprocessing (headless — no display required) |
| `tensorflow` | Neural network backend for DeepFace |
| `supabase` | Client for Supabase DB and Storage |
| `python-dotenv` | Loads environment variables from `.env` file |
| `bcrypt` | Password hashing for admin authentication |

### 4. Configure environment variables

Copy `.env.example` to `.env` and fill in your Supabase credentials:

```env
CONFIDENCE_THRESHOLD=0.6
SUPABASE_URL=https://your-project.supabase.co
SUPABASE_KEY=your-anon-key-here
SUPABASE_SERVICE_KEY=your-service-role-key-here
```

Get both keys from: **Supabase dashboard → Project Settings → API**

### 5. Run the gateway server

```bash
cd gateway
uvicorn app:app --host 0.0.0.0 --port 8000
```

Open `http://localhost:8000` in a browser. The web UI will load automatically.

On first load, click **Admin** to create your admin account (one-time setup). After that, the admin panel is locked to that account only.

> **Note:** First run downloads the ArcFace model weights (~137 MB). Subsequent runs load from cache. The embedding cache builds in the background on startup and takes ~15 min — see [Embedding Cache](#embedding-cache) below.

---

## Cloud Deployment

The gateway runs on Google Cloud Run. To deploy a new version after making changes:

```bash
# 1. Build image — runs DeepFace on enrolled/ at build time (~5-10 min)
gcloud builds submit --config cloudbuild.yaml .

# 2. Deploy the built image
gcloud run deploy glasstint-gateway \
  --image us-central1-docker.pkg.dev/elemental-vent-492005-u1/cloud-run-source-deploy/glasstint-gateway \
  --region us-central1 --allow-unauthenticated --memory 2Gi --timeout 300 --cpu-boost \
  --set-env-vars "SUPABASE_URL=...,SUPABASE_KEY=...,SUPABASE_SERVICE_KEY=...,CONFIDENCE_THRESHOLD=0.6"
```

The Docker build (`Dockerfile`) pre-computes ArcFace embeddings for everyone in `enrolled/` via `precompute_embeddings.py` and bakes them into the image — so the deployed service loads embeddings instantly on every cold start.

---

## Supabase Setup

Run the following in **Supabase → SQL Editor** to create the required tables:

```sql
create table admin_users (
  id uuid primary key default gen_random_uuid(),
  username text unique not null,
  password_hash text not null,
  created_at timestamptz default now()
);

create table admin_sessions (
  id uuid primary key default gen_random_uuid(),
  admin_id uuid references admin_users(id) on delete cascade,
  token text unique not null,
  expires_at timestamptz not null,
  created_at timestamptz default now()
);

create table enrolled_people (
  id uuid primary key default gen_random_uuid(),
  name text unique not null,
  description text,
  created_at timestamptz default now()
);

create table enrolled_photos (
  id uuid primary key default gen_random_uuid(),
  person_id uuid references enrolled_people(id) on delete cascade,
  storage_path text not null,
  created_at timestamptz default now()
);

create table recognition_events (
  id uuid primary key default gen_random_uuid(),
  decision text,
  confidence float,
  latency_ms int,
  device_id text,
  model_version text,
  liveness_score float,
  timestamp timestamptz default now()
);
```

#### Storage bucket

1. Supabase dashboard → **Storage** → **New bucket**
2. Name: `enrolled-photos`, toggle **Public** → **OFF** (private)
3. Click **Create bucket**

---

## Enrolling People

---

> **Note for the GlassTint team:** Anthony, Nedas, and Natania are already enrolled in Supabase — photos are in the `enrolled-photos` bucket and rows exist in `enrolled_people` and `enrolled_photos`. You do not need to repeat the setup below unless redeploying from scratch.

---

**Adding a new trusted person (normal workflow):**

Use the Admin panel in the web UI — log in, then enter their name, a description (e.g. "Family", "Caregiver"), and upload 3–5 clear, well-lit front-facing photos. The admin panel uploads to Supabase Storage and creates all DB rows automatically.

After adding someone via the UI:
- They will be recognized after ~15-20 min (background thread on the running container catches up from Supabase)
- Or hit **POST `/reload-cache`** with your admin token to trigger an immediate refresh
- To make them instant on future cold starts: add their photos to `enrolled/` and redeploy

**Updating/replacing someone's photos:**

There is no partial-update flow — remove and re-add is the cleanest approach:

1. Admin panel → **Remove** the person's entry (deletes photos from Storage and DB)
2. Re-enroll with the full updated set of photos

**Setting up from scratch on a fresh Supabase project:**

If setting up a new Supabase project with existing local photos in `enrolled/`:

1. Create the tables and bucket as described above

2. Upload each person's photos into the bucket — one folder per person, name must match exactly:
   ```
   enrolled-photos/
     Anthony/
       Anthony1.jpg
       Anthony2.jpg
       Anthony3.jpg
     Nedas/
       Nedas1.jpg
       ...
   ```
   Via **Supabase dashboard → Storage → enrolled-photos → Upload files**. Filenames must not contain spaces.

3. Insert the people into the DB:
   ```sql
   insert into enrolled_people (name, description) values
     ('Anthony', 'Brother'),
     ('Nedas',   'Brother'),
     ('Natania', 'Sister');

   select id, name from enrolled_people;
   ```

4. Copy the UUIDs and insert the photo rows into `enrolled_photos`:
   ```sql
   insert into enrolled_photos (person_id, storage_path) values
     ('<anthony-uuid>', 'Anthony/Anthony1.jpg'),
     ('<anthony-uuid>', 'Anthony/Anthony2.jpg'),
     ...
   ```

5. Verify:
   ```sql
   select count(*) from enrolled_photos;
   ```

---

## Embedding Cache

Face recognition requires pre-computing an ArcFace embedding vector for each enrolled person's reference photo. There are two sources:

| Source | When loaded | Speed |
|--------|------------|-------|
| `enrolled/` local photos | Baked into Docker image at build time via `precompute_embeddings.py` | Instant on startup |
| Supabase Storage photos | Background thread runs once per container start | ~15-20 min |

The baked pickle handles the team's current enrolled people (Anthony, Natania, Nedas). Anyone added via the UI gets picked up by the background thread. Hit `/reload-cache` (admin token required) to force a refresh without restarting.

---

## API Endpoints

| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| `POST` | `/verify` | None | Submit image for face verification. Returns name, confidence, trusted flag, description, latency. |
| `GET` | `/health` | None | Server health check + enrolled people list |
| `GET` | `/cache-status` | None | Number of loaded embeddings and ready state |
| `GET` | `/enrolled` | None | List all enrolled people with description and photo count |
| `POST` | `/enrolled` | Token | Add a new trusted person with reference photos |
| `DELETE` | `/enrolled/{name}` | Token | Remove a person and all their photos |
| `POST` | `/reload-cache` | Token | Trigger background reload of embeddings from Supabase |
| `GET` | `/events` | None | Paginated recognition history from Supabase |
| `GET` | `/admin/status` | None | Check if admin account has been created |
| `POST` | `/admin/setup` | None | One-time admin account creation (locked after first use) |
| `POST` | `/admin/login` | None | Login, returns 24hr session token |
| `POST` | `/admin/logout` | Token | Invalidate session token |
| `POST` | `/admin/change-password` | Token | Update admin password (invalidates all other sessions) |

### Verify response format

```json
{
  "result": "Anthony",
  "trusted": true,
  "description": "Brother",
  "confidence": 0.94,
  "latency_ms": 1800
}
```

Unknown face:

```json
{
  "result": "unknown",
  "trusted": false,
  "description": null,
  "confidence": 0.0,
  "latency_ms": 1200
}
```

---

## MCU-A Setup (Freenove ESP32-WROVER)

MCU-A handles all vision and networking: captures JPEG on ESP-NOW trigger from MCU-B, POSTs to cloud gateway, sends result back to MCU-B via ESP-NOW.

### 1. Board settings (Arduino IDE)

- **Board:** `ESP32 Wrover Module`
- **Partition Scheme:** `Huge APP (3MB No OTA)`
- **Upload Speed:** 115200

### 2. WiFi credentials

Arduino cannot read `.env` files. Credentials live in a gitignored `secrets.h` file:

```bash
# In capture_and_send/
cp secrets.h.example secrets.h
# Edit secrets.h and fill in your WiFi network
```

```cpp
// secrets.h — do not commit
#define WIFI_SSID     "your-network-name"
#define WIFI_PASSWORD "your-password"
```

Arduino IDE automatically includes all `.h` files in the sketch folder — `secrets.h` is available to the sketch without any extra steps.

### 3. MCU-B MAC address

Once your teammate sends you MCU-B's MAC address, fill it in at the top of `capture_and_send.ino`:

```cpp
uint8_t MCU_B_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
```

Leave all `0xFF` for **Solo Test Mode** — type anything in Serial Monitor to trigger a capture. Result prints to Serial only, no ESP-NOW send needed.

### 4. Wiring (MCU-A)

MCU-A is camera-only. The OV2640 camera module connects via the ribbon cable to the Freenove WROVER's ZIF connector — no extra wiring needed beyond the ribbon.

| Camera module pin | Connect to |
|-------------------|------------|
| OV2640 ribbon     | WROVER ZIF connector (latch must be locked) |

### 5. Gateway URL

The sketch points to the live cloud gateway by default — no changes needed. For local testing, update line 16 in `capture_and_send.ino`:

```cpp
const char* GATEWAY_URL = "http://YOUR_LAPTOP_IP:8000/verify";
```

---

## MCU-B Setup (Audio/Control ESP32)

MCU-B handles user input and feedback: button press triggers a CAPTURE command to MCU-A via ESP-NOW, and receives the result back to drive LED and audio.

### 1. Wiring (MCU-B)

| Component | GPIO | Notes |
|-----------|------|-------|
| Trigger button | GPIO 12 | Button to GND, internal pull-up |
| Status LED | GPIO 4 | LED + 330Ω resistor to GND |
| Speaker + amplifier | GPIO 25 (DAC) | DAC out → amplifier input → speaker |
| Battery monitor | GPIO 34 (ADC) | Voltage divider from LiPo+ |
| LiPo battery | VIN / GND | Via USB-C charge module |

### 2. ESP-NOW pairing

MCU-A and MCU-B communicate over ESP-NOW (direct 2.4 GHz radio, no router needed). Both boards must exchange MAC addresses:

1. Flash MCU-A — Serial Monitor prints `MCU-A MAC: XX:XX:XX:XX:XX:XX`
2. Flash MCU-B — Serial Monitor prints `MCU-B MAC: XX:XX:XX:XX:XX:XX`
3. Put MCU-A's MAC into MCU-B's sketch and MCU-B's MAC into MCU-A's `MCU_B_MAC` array
4. Reflash both

Both boards must be on the same WiFi channel. Set `peerInfo.channel = 0` on both (auto-follows WiFi channel).

---

## Security Notes

### Current setup (demo)

The `/verify` endpoint accepts image uploads from any client that can reach the Cloud Run URL. Acceptable for a demo — Cloud Run enforces HTTPS for all traffic.

### Production hardening

1. **Device API key** — add a pre-shared key header check on `/verify` so only authorized ESP32 devices can submit:
   ```python
   if x_api_key != os.getenv("DEVICE_API_KEY"):
       raise HTTPException(status_code=401)
   ```
   The ESP32 would include `X-Api-Key: <key>` in its POST.

2. **Rate limiting** — limit verify requests per device ID to prevent abuse.

The admin panel already uses token-based authentication (bcrypt + 24-hour session tokens). Only one admin account can ever be created — setup endpoint is permanently locked after first use.

---

## Project Structure

```
FullStackIoT/
  gateway/
    app.py                    # FastAPI server — verification, enrollment, admin auth
  frontend/
    index.html                # Web UI — verify tab, history tab, admin panel
  enrolled/
    [Name]/                   # Local reference photos (baked into Docker image at build time)
  capture_and_send/
    capture_and_send.ino      # MCU-A firmware — camera, HTTPS POST, ESP-NOW
    secrets.h.example         # WiFi credentials template (copy to secrets.h, gitignored)
  Dockerfile                  # Cloud Run container — pre-computes embeddings at build time
  cloudbuild.yaml             # Cloud Build config — pushes image to Artifact Registry
  Procfile                    # Startup command for local/cloud
  precompute_embeddings.py    # Runs during Docker build to bake embeddings into image
  requirements.txt            # Python dependencies
  .env.example                # Environment variable template
```

---

## Quick Test (No Hardware)

```bash
# 1. Activate venv and start server
source venv/bin/activate
cd gateway
uvicorn app:app --host 0.0.0.0 --port 8000

# 2. Open http://localhost:8000
# 3. Drop a photo into the Verify tab and click Run Verification
# 4. Or test via curl:
curl -X POST -F "image=@path/to/photo.jpg" http://localhost:8000/verify
```

Or test against the live cloud URL directly — no local setup needed.

---

## Troubleshooting

| Issue | Fix |
|-------|-----|
| `/verify` returns `unknown` right after cold start | Normal — wait 15-20 min for background thread, or hit `/reload-cache` after logging in |
| `/cache-status` shows 0 embeddings | Background thread still running or crashed — check Cloud Run logs |
| `ModuleNotFoundError: No module named 'cv2'` | Run `pip install opencv-python-headless` (not `opencv-python`) |
| DeepFace model download hangs | Check internet; ArcFace weights are ~137 MB, first run is slow |
| `(trapped) error reading bcrypt version` | Old passlib/bcrypt conflict — ensure `passlib` is not installed, only `bcrypt>=4.0.0` |
| ESP32 won't connect to Wi-Fi | Check `secrets.h` credentials; ensure 2.4 GHz network (not 5 GHz) |
| `ConnectionRefused` from ESP32 | Check laptop firewall; ensure uvicorn binds to `0.0.0.0` not `127.0.0.1` |
| ESP32 HTTPS connection fails | Confirm `WiFiClientSecure.setInsecure()` is called before `http.begin()` — already done in sketch |
| `CAMERA_INIT_FAILED` on ESP32 | Check ribbon cable is fully seated and ZIF latch is locked; power cycle |
| Low confidence scores | Add more enrolled photos with varied lighting and angles (3-5 minimum) |
| Storage photos `not_found` | Filenames in `enrolled_photos` table must exactly match filenames in Supabase Storage bucket |
| Description not appearing in results | Hard refresh browser (Cmd+Shift+R) to clear JS cache |
| Cloud Build fails at Dockerfile parse | Multi-line `python -c "..."` breaks Cloud Build's Docker parser — use a `.py` file instead (see `precompute_embeddings.py`) |
| `gcr.io repo does not exist` on deploy | Use `cloudbuild.yaml` which pushes to Artifact Registry (`us-central1-docker.pkg.dev`) |

---

## Hardware BOM

| Part | Purpose | Cost |
|------|---------|------|
| Freenove ESP32-WROVER Kit | MCU-A — camera + WiFi | $0 (from kit) |
| ESP32 Dev Board | MCU-B — button, LED, audio, battery | $0 (from kit) |
| OV2640 Camera Module | Image capture (ribbon to WROVER) | $0 (from kit) |
| Speaker + amplifier | Audio feedback | $0 (from kit) |
| Tactile buttons | User input | $0 (from kit) |
| Glasses frame | Wearable mount | $10 |
| LiPo battery | Portable power | $10 |
| USB-C charge module | Battery charging | $6 |
| 3D print material | Camera mount for glasses | $5 |
| Enclosure accessories | Protection and mounting | $7 |
| **Total** | | **$38** |

---

## Team

- Nedas Jaronis — jaronisnedas@ufl.edu
- Anthony D. Madorsky — anthonymadorsky@ufl.edu
- Natania Philippe — nphilippe@ufl.edu
- Hemdutt Rao — raohemdutt@ufl.edu

University of Florida

## License

MIT
