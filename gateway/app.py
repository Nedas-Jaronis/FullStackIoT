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
    best_confidence = 0.0

    for person_dir in ENROLLED_DIR.iterdir():
        if not person_dir.is_dir():
            continue

        for ref_image in person_dir.glob("*.jpg"):
            try:
                result = DeepFace.verify(
                    img1_path=image_path,
                    img2_path=str(ref_image),
                    model_name="ArcFace", #Our selected model its Best accuracy and medium speed
                    enforce_detection=False,
                )
                confidence = 1 - result["distance"]
                if confidence > best_confidence:
                    best_confidence = confidence
                    best_name = person_dir.name
            except Exception:
                continue

    if best_confidence < CONFIDENCE_THRESHOLD:
        return "unknown", best_confidence

    return best_name, best_confidence


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
