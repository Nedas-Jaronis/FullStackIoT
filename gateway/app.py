import os
import time
import tempfile
from pathlib import Path

from dotenv import load_dotenv
from fastapi import FastAPI, UploadFile, File
from fastapi.responses import JSONResponse
from deepface import DeepFace # Our model (facebook open source model)
from supabase import create_client

load_dotenv()

app = FastAPI(title="GlassTint Gateway Connection")

ENROLLED_DIR = Path(__file__).resolve().parent.parent / "enrolled"
CONFIDENCE_THRESHOLD = float(os.getenv("CONFIDENCE_THRESHOLD", "0.6"))
SUPABASE_URL = os.getenv("SUPABASE_URL")
SUPABASE_KEY = os.getenv("SUPABASE_KEY")

supabase = None
if SUPABASE_URL and SUPABASE_KEY:
    supabase = create_client(SUPABASE_URL, SUPABASE_KEY)


def find_match(image_path):
    best_name = "unknown"
    best_distance = float("inf")

    for person_dir in ENROLLED_DIR.iterdir():
        if not person_dir.is_dir():
            continue

        for ref_image in person_dir.glob("*.jpg"):
            try:
                result = DeepFace.verify(
                    img1_path=image_path,
                    img2_path=str(ref_image),
                    model_name="ArcFace", #Our selected model its Best accuracy and medium speed
                    enforce_detection=True,
                )
                distance = result["distance"]
                threshold = result["threshold"]
                verified = result["verified"]
                print(f"  {person_dir.name}/{ref_image.name}: distance={distance:.4f}, threshold={threshold}, verified={verified}")

                # only consider verified matches, then pick the closest one
                if verified and distance < best_distance:
                    best_distance = distance
                    best_name = person_dir.name
            except Exception as e:
                print(f"  skipping {ref_image}: {e}")
                continue

    if best_name == "unknown":
        return "unknown", 0.0

    # convert distance to a confidence score (lower distance = higher confidence)
    confidence = max(0, 1 - best_distance)
    return best_name, confidence


def log_event(decision, confidence, latency_ms):
    if not supabase:
        return
    try:
        supabase.table("recognition_events").insert({ #Our database schema taking in decision, confidence and latency
            "decision": decision,
            "confidence": round(confidence, 4),
            "latency_ms": latency_ms,
        }).execute()
    except Exception as e:
        print(f"Supabase logging failed: {e}")

# A endpoint that verifies against our enrolled photos
@app.post("/verify")
async def verify(image: UploadFile = File(...)):
    start = time.time()

    # Save uploaded image to a temp file
    with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as tmp:
        tmp.write(await image.read())
        tmp_path = tmp.name

    try:
        name, confidence = find_match(tmp_path)
        latency_ms = int((time.time() - start) * 1000)

        log_event(name, confidence, latency_ms)

        return JSONResponse(content={
            "result": name,
            "confidence": round(confidence, 4),
            "latency_ms": latency_ms,
        })
    finally:
        os.unlink(tmp_path)


@app.get("/health")
async def health():
    enrolled = [d.name for d in ENROLLED_DIR.iterdir() if d.is_dir()]
    return {"status": "ok", "enrolled_people": enrolled}
