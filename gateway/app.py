import os
import time
import tempfile
import secrets
from datetime import datetime, timezone, timedelta
from pathlib import Path
from typing import List, Optional

from dotenv import load_dotenv
from fastapi import FastAPI, UploadFile, File, Header, Query, HTTPException, Form, Depends
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import bcrypt
import threading
import numpy as np
from deepface import DeepFace
from supabase import create_client

load_dotenv()

# ── Config ──────────────────────────────────────────────────────────────────────
ENROLLED_DIR = Path(__file__).resolve().parent.parent / "enrolled"
FRONTEND_DIR = Path(__file__).resolve().parent.parent / "frontend"
CONFIDENCE_THRESHOLD = float(os.getenv("CONFIDENCE_THRESHOLD", "0.6"))
SUPABASE_URL = os.getenv("SUPABASE_URL")
SUPABASE_KEY = os.getenv("SUPABASE_KEY")
SUPABASE_SERVICE_KEY = os.getenv("SUPABASE_SERVICE_KEY")
MODEL_VERSION = "ArcFace"
STORAGE_BUCKET = "enrolled-photos"
TOKEN_TTL_HOURS = 24

def _hash_password(password: str) -> str:
    return bcrypt.hashpw(password.encode(), bcrypt.gensalt()).decode()

def _check_password(password: str, hashed: str) -> bool:
    return bcrypt.checkpw(password.encode(), hashed.encode())

# Anon client — used for recognition_events reads
supabase = None
if SUPABASE_URL and SUPABASE_KEY:
    supabase = create_client(SUPABASE_URL, SUPABASE_KEY)

# Service role client — used for all admin + storage operations (bypasses RLS)
supabase_admin = None
if SUPABASE_URL and SUPABASE_SERVICE_KEY:
    supabase_admin = create_client(SUPABASE_URL, SUPABASE_SERVICE_KEY)


# ── Auth helpers ────────────────────────────────────────────────────────────────
def _admin_exists() -> bool:
    if not supabase_admin:
        return False
    try:
        res = supabase_admin.table("admin_users").select("id").limit(1).execute()
        return bool(res.data)
    except Exception:
        return False


def _verify_credentials(username: str, password: str) -> Optional[str]:
    """Returns admin id if credentials are valid, None otherwise."""
    if not supabase_admin:
        return None
    try:
        row = (
            supabase_admin.table("admin_users")
            .select("id, password_hash")
            .eq("username", username)
            .single()
            .execute()
        )
        if row.data and _check_password(password, row.data["password_hash"]):
            return row.data["id"]
    except Exception:
        pass
    return None


def _create_session(admin_id: str) -> str:
    token = secrets.token_urlsafe(32)
    expires_at = (datetime.now(timezone.utc) + timedelta(hours=TOKEN_TTL_HOURS)).isoformat()
    supabase_admin.table("admin_sessions").insert({
        "admin_id": admin_id,
        "token": token,
        "expires_at": expires_at,
    }).execute()
    return token


def _verify_token(token: str) -> bool:
    if not supabase_admin or not token:
        return False
    try:
        now = datetime.now(timezone.utc).isoformat()
        res = (
            supabase_admin.table("admin_sessions")
            .select("id")
            .eq("token", token)
            .gt("expires_at", now)
            .limit(1)
            .execute()
        )
        return bool(res.data)
    except Exception:
        return False


def require_auth(authorization: str = Header(default="")) -> str:
    token = authorization.removeprefix("Bearer ").strip()
    if not _verify_token(token):
        raise HTTPException(status_code=401, detail="Unauthorized")
    return token


# ── Embedding cache ────────────────────────────────────────────────────────────
# stores pre-computed embeddings so we don't reprocess enrolled photos every scan
# each entry: {"name": "Nedas", "embedding": np.array, "source": "Nedas/photo1.jpg"}
embedding_cache = []


def _download_enrolled(tmp_dir: str) -> bool:
    """Download all reference photos from Supabase Storage into tmp_dir."""
    if not supabase_admin:
        return False
    try:
        people = supabase_admin.table("enrolled_people").select("id, name").execute().data
        if not people:
            return False
        downloaded = 0
        for person in people:
            photos = (
                supabase_admin.table("enrolled_photos")
                .select("storage_path")
                .eq("person_id", person["id"])
                .execute()
                .data
            )
            person_dir = Path(tmp_dir) / person["name"]
            person_dir.mkdir(exist_ok=True)
            for photo in photos:
                storage_path = photo["storage_path"]
                try:
                    data = supabase_admin.storage.from_(STORAGE_BUCKET).download(storage_path)
                    dest = person_dir / Path(storage_path).name
                    dest.write_bytes(data)
                    downloaded += 1
                except Exception as e:
                    print(f"  failed to download {storage_path}: {e}")
        return downloaded > 0
    except Exception as e:
        print(f"Supabase download error: {e}")
        return False


def build_embedding_cache(enrolled_dir: Path):
    """Pre-compute embeddings for all enrolled photos. Call on startup or after enrollment changes."""
    global embedding_cache
    new_cache = []
    print(f"Building embedding cache from {enrolled_dir}...")

    for person_dir in enrolled_dir.iterdir():
        if not person_dir.is_dir():
            continue
        for ref_image in person_dir.glob("*.jpg"):
            try:
                # extract the face embedding vector
                reps = DeepFace.represent(
                    img_path=str(ref_image),
                    model_name=MODEL_VERSION,
                    enforce_detection=True,
                )
                if reps:
                    new_cache.append({
                        "name": person_dir.name,
                        "embedding": np.array(reps[0]["embedding"]),
                        "source": f"{person_dir.name}/{ref_image.name}",
                    })
                    print(f"  cached {person_dir.name}/{ref_image.name}")
            except Exception as e:
                import traceback
                print(f"  skipping {ref_image.name}: {e}")
                print(traceback.format_exc())

    embedding_cache = new_cache
    print(f"Cache ready: {len(embedding_cache)} embeddings for {len(set(e['name'] for e in embedding_cache))} people")


def load_and_cache_embeddings():
    """Download from Supabase or use local enrolled/, then build cache."""
    with tempfile.TemporaryDirectory() as tmp_dir:
        if _download_enrolled(tmp_dir):
            build_embedding_cache(Path(tmp_dir))
        else:
            print("Supabase storage unavailable — falling back to local enrolled/")
            build_embedding_cache(ENROLLED_DIR)


def find_match(image_path: str):
    if not embedding_cache:
        return "unknown", 0.0

    # get embedding for the uploaded image — no face detected means unknown
    try:
        reps = DeepFace.represent(
            img_path=image_path,
            model_name=MODEL_VERSION,
            enforce_detection=True,
        )
    except ValueError:
        print("  no face detected in uploaded image")
        return "unknown", 0.0

    if not reps:
        return "unknown", 0.0

    test_embedding = np.array(reps[0]["embedding"])

    # compare against all cached embeddings using cosine distance
    best_name = "unknown"
    best_distance = float("inf")

    for entry in embedding_cache:
        # cosine distance
        dot = np.dot(test_embedding, entry["embedding"])
        norm = np.linalg.norm(test_embedding) * np.linalg.norm(entry["embedding"])
        distance = 1 - (dot / norm) if norm > 0 else 1.0
        confidence = max(0.0, 1 - distance)
        print(f"  {entry['source']}: distance={distance:.4f}, confidence={confidence:.1%}")

        if distance < CONFIDENCE_THRESHOLD and distance < best_distance:
            best_distance = distance
            best_name = entry["name"]

    if best_name == "unknown":
        return "unknown", 0.0
    return best_name, max(0.0, 1 - best_distance)


def log_event(decision, confidence, latency_ms, device_id="web-ui"):
    if not supabase:
        return
    try:
        supabase.table("recognition_events").insert({
            "decision": decision,
            "confidence": round(confidence, 4),
            "latency_ms": latency_ms,
            "device_id": device_id,
            "model_version": MODEL_VERSION,
            "liveness_score": None,
        }).execute()
    except Exception as e:
        print(f"Supabase logging failed: {e}")


# ── App ──────────────────────────────────────────────────────────────────────────
app = FastAPI(title="GlassTint Gateway")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# build embedding cache in background so server starts immediately
# (Cloud Run needs port 8080 open before loading embeddings)
@app.on_event("startup")
async def startup():
    global embedding_cache
    # Load pre-computed embeddings from Docker build (instant, no GPU needed)
    pkl = Path(__file__).resolve().parent.parent / "enrolled_embeddings.pkl"
    if pkl.exists():
        import pickle
        raw = pickle.load(open(pkl, "rb"))
        embedding_cache = [
            {"name": e["name"], "embedding": np.array(e["embedding"]), "source": e["source"]}
            for e in raw
        ]
        print(f"Loaded {len(embedding_cache)} pre-computed embeddings from pickle")
    # Update from Supabase in background to pick up any new enrollments
    thread = threading.Thread(target=load_and_cache_embeddings, daemon=True)
    thread.start()


@app.get("/cache-status")
async def cache_status():
    return {"cached_embeddings": len(embedding_cache), "ready": len(embedding_cache) > 0}


# endpoint to reload cache after enrolling new people
@app.post("/reload-cache")
async def reload_cache(_=Depends(require_auth)):
    thread = threading.Thread(target=load_and_cache_embeddings, daemon=True)
    thread.start()
    return {"status": "reloading", "current_cached": len(embedding_cache), "message": "Reload started in background. Poll /cache-status to track progress."}


# ── Verification ─────────────────────────────────────────────────────────────────
@app.post("/verify")
async def verify(
    image: UploadFile = File(...),
    x_device_id: str = Header(default="web-ui"),
):
    start = time.time()
    with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as tmp:
        tmp.write(await image.read())
        tmp_path = tmp.name
    try:
        name, confidence = find_match(tmp_path)
        latency_ms = int((time.time() - start) * 1000)
        log_event(name, confidence, latency_ms, device_id=x_device_id)

        description = None
        if name != "unknown" and supabase_admin:
            try:
                row = (
                    supabase_admin.table("enrolled_people")
                    .select("description")
                    .eq("name", name)
                    .single()
                    .execute()
                )
                if row.data:
                    description = row.data["description"]
            except Exception:
                pass

        return JSONResponse(content={
            "result": name,
            "confidence": round(confidence, 4),
            "latency_ms": latency_ms,
            "trusted": name != "unknown",
            "description": description,
        })
    finally:
        os.unlink(tmp_path)


@app.get("/health")
async def health():
    people = []
    if supabase_admin:
        try:
            res = supabase_admin.table("enrolled_people").select("name").execute()
            people = [r["name"] for r in res.data]
        except Exception:
            pass
    if not people and ENROLLED_DIR.exists():
        people = [d.name for d in ENROLLED_DIR.iterdir() if d.is_dir()]
    return {"status": "ok", "enrolled_people": people}


# ── Enrolled people ───────────────────────────────────────────────────────────────
@app.get("/enrolled")
async def get_enrolled():
    """List enrolled trusted people from Supabase, falling back to local directory."""
    if supabase_admin:
        try:
            people_res = supabase_admin.table("enrolled_people").select("id, name, description").order("name").execute()
            people = []
            for p in people_res.data:
                photos_res = (
                    supabase_admin.table("enrolled_photos")
                    .select("id")
                    .eq("person_id", p["id"])
                    .execute()
                )
                people.append({
                    "name": p["name"],
                    "description": p["description"],
                    "photo_count": len(photos_res.data),
                })
            return {"people": people}
        except Exception as e:
            print(f"Supabase enrolled fetch failed: {e}")

    # Local fallback
    people = []
    if ENROLLED_DIR.exists():
        for person_dir in sorted(ENROLLED_DIR.iterdir()):
            if person_dir.is_dir():
                meta_file = person_dir / "meta.txt"
                people.append({
                    "name": person_dir.name,
                    "description": meta_file.read_text().strip() if meta_file.exists() else None,
                    "photo_count": len(list(person_dir.glob("*.jpg"))),
                })
    return {"people": people}


@app.post("/enrolled")
async def add_enrolled(
    name: str = Form(...),
    description: Optional[str] = Form(default=""),
    photos: List[UploadFile] = File(...),
    _token: str = Depends(require_auth),
):
    """Add a new trusted person with reference photos. Requires admin token."""
    if not supabase_admin:
        raise HTTPException(status_code=503, detail="Supabase not configured")

    safe_name = "".join(c for c in name if c.isalnum() or c in (" ", "-", "_")).strip()
    if not safe_name:
        raise HTTPException(status_code=400, detail="Invalid name")

    # Insert or get person record
    existing = (
        supabase_admin.table("enrolled_people")
        .select("id")
        .eq("name", safe_name)
        .limit(1)
        .execute()
    )
    if existing.data:
        person_id = existing.data[0]["id"]
    else:
        insert_res = supabase_admin.table("enrolled_people").insert({
            "name": safe_name,
            "description": description or None,
        }).execute()
        person_id = insert_res.data[0]["id"]

    saved = 0
    for photo in photos:
        ext = Path(photo.filename).suffix.lower() if photo.filename else ".jpg"
        if ext not in {".jpg", ".jpeg", ".png"}:
            continue
        contents = await photo.read()
        storage_path = f"{safe_name}/{safe_name}{saved + 1}.jpg"
        try:
            supabase_admin.storage.from_(STORAGE_BUCKET).upload(
                storage_path,
                contents,
                {"content-type": "image/jpeg", "upsert": "true"},
            )
            supabase_admin.table("enrolled_photos").insert({
                "person_id": person_id,
                "storage_path": storage_path,
            }).execute()
            saved += 1
        except Exception as e:
            print(f"Failed to upload {storage_path}: {e}")

    if saved == 0:
        raise HTTPException(status_code=400, detail="No valid images uploaded")

    return {"ok": True, "name": safe_name, "photos_saved": saved}


@app.get("/enrolled/{name}/photos")
async def get_enrolled_photos(name: str):
    """Get all photos for an enrolled person with viewable URLs."""
    if not supabase_admin:
        raise HTTPException(status_code=503, detail="Supabase not configured")

    person = (
        supabase_admin.table("enrolled_people")
        .select("id")
        .eq("name", name)
        .limit(1)
        .execute()
    )
    if not person.data:
        raise HTTPException(status_code=404, detail="Person not found")

    photos = (
        supabase_admin.table("enrolled_photos")
        .select("id, storage_path")
        .eq("person_id", person.data[0]["id"])
        .execute()
    )

    photo_list = []
    for p in photos.data:
        try:
            url_res = supabase_admin.storage.from_(STORAGE_BUCKET).create_signed_url(
                p["storage_path"], 3600
            )
            photo_list.append({
                "id": p["id"],
                "storage_path": p["storage_path"],
                "url": url_res["signedURL"],
            })
        except Exception as e:
            print(f"Failed to get signed URL for {p['storage_path']}: {e}")

    return {"name": name, "photos": photo_list}


@app.delete("/enrolled/{name}/photos/{photo_id}")
async def delete_enrolled_photo(name: str, photo_id: str, _token: str = Depends(require_auth)):
    """Delete a single photo from an enrolled person."""
    if not supabase_admin:
        raise HTTPException(status_code=503, detail="Supabase not configured")

    photo = (
        supabase_admin.table("enrolled_photos")
        .select("storage_path")
        .eq("id", photo_id)
        .single()
        .execute()
    )
    if not photo.data:
        raise HTTPException(status_code=404, detail="Photo not found")

    try:
        supabase_admin.storage.from_(STORAGE_BUCKET).remove([photo.data["storage_path"]])
    except Exception as e:
        print(f"Failed to delete from storage: {e}")

    supabase_admin.table("enrolled_photos").delete().eq("id", photo_id).execute()
    return {"ok": True, "deleted_photo_id": photo_id}


@app.delete("/enrolled/{name}")
async def delete_enrolled(name: str, _token: str = Depends(require_auth)):
    """Remove a trusted person and all their reference photos."""
    if not supabase_admin:
        raise HTTPException(status_code=503, detail="Supabase not configured")

    person = (
        supabase_admin.table("enrolled_people")
        .select("id")
        .eq("name", name)
        .single()
        .execute()
    )
    if not person.data:
        raise HTTPException(status_code=404, detail="Person not found")

    person_id = person.data["id"]

    # Delete photos from Storage
    photos = (
        supabase_admin.table("enrolled_photos")
        .select("storage_path")
        .eq("person_id", person_id)
        .execute()
    )
    for photo in photos.data:
        try:
            supabase_admin.storage.from_(STORAGE_BUCKET).remove([photo["storage_path"]])
        except Exception as e:
            print(f"Failed to delete {photo['storage_path']} from storage: {e}")

    # Delete DB record (enrolled_photos cascade deletes automatically)
    supabase_admin.table("enrolled_people").delete().eq("id", person_id).execute()

    return {"ok": True, "deleted": name}


# ── Recognition history ───────────────────────────────────────────────────────────
@app.get("/events")
async def get_events(
    limit: int = Query(default=50, le=200),
    offset: int = Query(default=0),
):
    if not supabase:
        return {"events": [], "error": "Supabase not configured"}
    try:
        response = (
            supabase.table("recognition_events")
            .select("*")
            .order("timestamp", desc=True)
            .range(offset, offset + limit - 1)
            .execute()
        )
        return {"events": response.data}
    except Exception as e:
        return {"events": [], "error": str(e)}


# ── Admin auth ────────────────────────────────────────────────────────────────────
class AdminSetupRequest(BaseModel):
    username: str
    password: str


class AdminLoginRequest(BaseModel):
    username: str
    password: str


class AdminChangePasswordRequest(BaseModel):
    current_password: str
    new_password: str


@app.get("/admin/status")
async def admin_status():
    return {"setup_required": not _admin_exists()}


@app.post("/admin/setup")
async def admin_setup(body: AdminSetupRequest):
    """One-time endpoint to create the first admin account."""
    if not supabase_admin:
        raise HTTPException(status_code=503, detail="Supabase not configured")
    if _admin_exists():
        raise HTTPException(status_code=403, detail="Admin already configured")
    if not body.username or len(body.password) < 8:
        raise HTTPException(status_code=400, detail="Username required and password must be at least 8 characters")
    try:
        res = supabase_admin.table("admin_users").insert({
            "username": body.username,
            "password_hash": _hash_password(body.password),
        }).execute()
        token = _create_session(res.data[0]["id"])
        return {"ok": True, "token": token}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/admin/login")
async def admin_login(body: AdminLoginRequest):
    admin_id = _verify_credentials(body.username, body.password)
    if not admin_id:
        raise HTTPException(status_code=403, detail="Invalid credentials")
    token = _create_session(admin_id)
    return {"ok": True, "token": token}


@app.post("/admin/logout")
async def admin_logout(authorization: str = Header(default="")):
    token = authorization.removeprefix("Bearer ").strip()
    if token and supabase_admin:
        try:
            supabase_admin.table("admin_sessions").delete().eq("token", token).execute()
        except Exception:
            pass
    return {"ok": True}


@app.post("/admin/change-password")
async def admin_change_password(
    body: AdminChangePasswordRequest,
    authorization: str = Header(default=""),
):
    token = authorization.removeprefix("Bearer ").strip()
    if not _verify_token(token):
        raise HTTPException(status_code=401, detail="Unauthorized")
    if len(body.new_password) < 8:
        raise HTTPException(status_code=400, detail="New password must be at least 8 characters")

    # Get admin_id from session
    session = (
        supabase_admin.table("admin_sessions")
        .select("admin_id")
        .eq("token", token)
        .single()
        .execute()
    )
    admin_id = session.data["admin_id"]

    # Verify current password
    admin = (
        supabase_admin.table("admin_users")
        .select("password_hash")
        .eq("id", admin_id)
        .single()
        .execute()
    )
    if not _check_password(body.current_password, admin.data["password_hash"]):
        raise HTTPException(status_code=403, detail="Current password is incorrect")

    supabase_admin.table("admin_users").update({
        "password_hash": _hash_password(body.new_password),
    }).eq("id", admin_id).execute()

    # Invalidate all other sessions
    supabase_admin.table("admin_sessions").delete().eq("admin_id", admin_id).neq("token", token).execute()

    return {"ok": True}


# ── Static frontend — mount last so API routes take priority ───────────────────
if FRONTEND_DIR.exists():
    app.mount("/", StaticFiles(directory=str(FRONTEND_DIR), html=True), name="frontend")
