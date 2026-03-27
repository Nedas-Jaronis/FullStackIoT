# GlassTint

IoT-enabled assistive glasses for trusted person recognition. Helps visually impaired and memory-impaired users identify nearby people through wearable face verification with audio feedback.

**Architecture:** ESP32-CAM captures a face on button press, sends it to a laptop gateway running DeepFace verification, and an audio ESP32 speaks the result. Enrolled people and photos are stored in Supabase. Cloud logging via Supabase.

## Prerequisites

- Python 3.9–3.11 (3.12+ may have compatibility issues with some dependencies)
- pip
- Git
- Arduino IDE 2.x (for ESP32 firmware)
- A webcam or ESP32-CAM module
- Supabase account (free tier) for cloud storage and logging

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
pip install -r requirements.txt
```

**What each package does:**

| Package | Purpose |
|---------|---------|
| `fastapi` | Async HTTP server that receives images from ESP32-CAM and returns results |
| `uvicorn` | ASGI server to run FastAPI |
| `python-multipart` | Required for file upload handling in FastAPI |
| `deepface` | Face detection, embedding extraction, and verification (ArcFace model) |
| `opencv-python` | Image preprocessing |
| `supabase` | Client for Supabase DB and Storage |
| `python-dotenv` | Loads environment variables from `.env` file |
| `bcrypt` | Password hashing for admin authentication |

DeepFace will automatically download model weights on first run (~100–500 MB). Default model is ArcFace.

### 4. Configure environment variables

Copy `.env.example` to `.env` and fill in your Supabase credentials:

```env
PORT=5000
CONFIDENCE_THRESHOLD=0.6
SUPABASE_URL=https://your-project.supabase.co
SUPABASE_KEY=your-anon-key-here
SUPABASE_SERVICE_KEY=your-service-role-key-here
```

Get both keys from: **Supabase dashboard → Project Settings → API**

### 5. Supabase Setup

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

-- recognition_events table should already exist from initial setup
```

Also create the `recognition_events` table if it doesn't exist yet:

```sql
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

#### Enrolling people

---

> **Note for the GlassTint team:** The three initial team members (Anthony, Nedas, Natania) have already been enrolled in Supabase — photos are in the `enrolled-photos` bucket and rows exist in `enrolled_people` and `enrolled_photos`. You do not need to repeat the setup below unless you are redeploying from scratch. See the sections below for how to update your own photos or add new people.

---

**Adding a new trusted person (normal workflow):**

Use the Admin panel in the web UI — enter their name, description (e.g. "Family", "Caregiver"), and upload 3–5 clear, well-lit front-facing photos. The admin panel uploads to Supabase Storage and creates all DB rows automatically. Nothing else needed.

**Adding more photos of yourself / updating your photos:**

If you want to improve recognition accuracy by adding more reference photos, or replace existing ones:

1. Open the Admin panel → **Remove** your current entry (this deletes your photos from Storage and the DB)
2. Re-enroll yourself with the full updated set of photos using the Add Trusted Person form

There is no partial-update flow — remove and re-add is the cleanest approach.

**Replicating this setup on a fresh Supabase project (e.g. a new deployment):**

If you are setting up a new Supabase project from scratch with existing local photos in `enrolled/`:

1. Create the tables and bucket as described in steps above

2. Upload each person's photos into the bucket manually — create one folder per person matching their name exactly:
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
   Do this via **Supabase dashboard → Storage → enrolled-photos → Upload files**. Filenames must not contain spaces.

3. Insert the people into the DB, then get their UUIDs:
   ```sql
   insert into enrolled_people (name, description) values
     ('Anthony', 'your description here'),
     ('Nedas',   'your description here'),
     ('Natania', 'your description here');

   select id, name from enrolled_people;
   ```

4. Copy the UUIDs from step 3 into `supabase/seed_enrolled_photos.sql`, updating the paths to match your actual filenames, then run the file in the SQL Editor.

5. Verify the count matches your total number of uploaded photos:
   ```sql
   select count(*) from enrolled_photos;
   ```

The `enrolled/` local folder is kept as a **fallback** — if Supabase Storage is unreachable, the gateway automatically falls back to the local photos.

### 6. Run the gateway server

```bash
cd gateway
uvicorn app:app --host 0.0.0.0 --port 5000
```

Open `http://localhost:5000` in a browser. The web UI will load automatically.

On first load, click **🔐 Admin** to create your admin account (one-time setup). After that, the admin panel is locked to that account only — no one else can create an admin.

### 7. Enroll trusted people

Use the Admin panel in the web UI to add trusted people with reference photos and a description (e.g. "Family", "Caregiver", "Colleague"). Photos are uploaded to Supabase Storage automatically.

An `enrolled/` directory is kept as a local fallback in case Supabase Storage is unreachable.

## API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/verify` | Submit image for face verification. Returns name, confidence, trusted flag, description, and latency. |
| `GET` | `/health` | Server health check + enrolled people list |
| `GET` | `/enrolled` | List all enrolled people with description and photo count |
| `POST` | `/enrolled` | Add a new trusted person (admin token required) |
| `DELETE` | `/enrolled/{name}` | Remove a person and all their photos (admin token required) |
| `GET` | `/events` | Paginated recognition history from Supabase |
| `GET` | `/admin/status` | Check if admin account has been created |
| `POST` | `/admin/setup` | One-time admin account creation (locked after first use) |
| `POST` | `/admin/login` | Login, returns session token (24hr TTL) |
| `POST` | `/admin/logout` | Invalidate session token |
| `POST` | `/admin/change-password` | Update admin password (invalidates all other sessions) |

### Verify response format

```json
{
  "result": "Anthony",
  "trusted": true,
  "description": "Brother",
  "confidence": 0.82,
  "latency_ms": 3200
}
```

or for an unknown face:

```json
{
  "result": "unknown",
  "trusted": false,
  "description": null,
  "confidence": 0.0,
  "latency_ms": 2800
}
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

### 2. Wiring

- **Button:** GPIO 12 to GND (internal pull-up used)
- **LED indicator:** GPIO 4 (built-in flash LED)

## Audio MCU Setup (Track 3 — MCU-B)

### 1. Flash the second ESP32

1. Open `esp32audio/speak_result.ino` in Arduino IDE
2. Update Wi-Fi credentials
3. Select board: **Tools > Board > ESP32 Dev Module**
4. Upload

The audio MCU calls `/verify` with the captured image and speaks the result using TTS. The response includes `trusted`, `result` (name), and `description` — the spoken output is:
- **Trusted:** `"Trusted. [Name]. [Description]."`
- **Unknown:** `"Unknown person detected."`

### 2. Wiring

- **Speaker + amplifier:** DAC output (GPIO 25) to amplifier input
- **Amplifier output:** to speaker terminals
- **Button (optional):** GPIO 12 to GND

## Security Notes

### Current setup (development/demo)

The `/verify` endpoint accepts image uploads from any client that can reach port 5000. This is acceptable for a local demo where the gateway is on a private network.

### Production hardening

For a deployed or public-facing version, the recommended approach is:

1. **TLS via reverse proxy** — run nginx in front of uvicorn with a Let's Encrypt certificate so all traffic is encrypted:
   ```nginx
   server {
       listen 443 ssl;
       ssl_certificate /etc/letsencrypt/live/yourdomain/fullchain.pem;
       ssl_certificate_key /etc/letsencrypt/live/yourdomain/privkey.pem;
       location / { proxy_pass http://127.0.0.1:5000; }
   }
   ```

2. **Device API key** — add a pre-shared key header check on `/verify` so only authorized devices (ESP32s) can submit images:
   ```python
   # In the verify endpoint
   if x_api_key != os.getenv("DEVICE_API_KEY"):
       raise HTTPException(status_code=401)
   ```
   The ESP32 would include `X-Api-Key: <key>` in its POST request.

3. **Rate limiting** — limit verify requests per device to prevent abuse.

The admin panel already uses token-based authentication (bcrypt + 24-hour session tokens). Only one admin account can ever be created — the setup endpoint is permanently locked after first use.

## Project Structure

```
FullStackIoT/
  gateway/
    app.py              # FastAPI server — verification, enrolled management, admin auth
  frontend/
    index.html          # Web UI — verify tab, history tab, admin panel
  enrolled/
    [Name]/             # Local fallback reference photos (Supabase Storage is primary)
  supabase/
    seed_enrolled_photos.sql  # SQL for seeding enrolled_photos table manually
  .env                  # Environment config (not committed)
  .env.example          # Template for .env
  requirements.txt      # Python dependencies
```

## Quick Test (No Hardware)

```bash
# 1. Activate venv and start server
source venv/bin/activate
cd gateway
uvicorn app:app --host 0.0.0.0 --port 5000

# 2. Open http://localhost:5000
# 3. Drop a photo into the Verify tab and click Run Verification
# 4. Or test via curl:
curl -X POST -F "image=@path/to/photo.jpg" http://localhost:5000/verify
```

## Troubleshooting

| Issue | Fix |
|-------|-----|
| `ModuleNotFoundError: No module named 'cv2'` | Run `pip install opencv-python` |
| DeepFace model download hangs | Check internet connection; models are ~100–500 MB, first run is slow |
| `(trapped) error reading bcrypt version` | Old passlib/bcrypt conflict — ensure `passlib` is not installed, only `bcrypt>=4.0.0` |
| ESP32-CAM won't connect to Wi-Fi | Verify SSID/password; ensure 2.4 GHz network (not 5 GHz) |
| `ConnectionRefused` from ESP32 | Check laptop firewall; ensure uvicorn binds to `0.0.0.0` not `127.0.0.1` |
| Low confidence scores | Add more enrolled photos with varied lighting/angles |
| `CAMERA_INIT_FAILED` on ESP32-CAM | Check camera ribbon cable connection; try power cycling |
| Storage photos `not_found` | Filenames in `enrolled_photos` table must exactly match filenames in Supabase Storage bucket |
| Description not appearing in TTS | Hard refresh browser (Cmd+Shift+R) to clear JS cache |

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
