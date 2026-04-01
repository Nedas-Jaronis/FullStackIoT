FROM python:3.11-slim

WORKDIR /app

# System deps required by opencv-headless and deepface
RUN apt-get update && apt-get install -y \
    libglib2.0-0 \
    libsm6 \
    libxext6 \
    libxrender-dev \
    libgomp1 \
    libgl1 \
    wget \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt .
RUN pip install --upgrade pip setuptools wheel
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

# Pre-download ArcFace model weights
RUN mkdir -p /root/.deepface/weights && \
    wget -q "https://github.com/serengil/deepface_models/releases/download/v1.0/arcface_weights.h5" \
    -O /root/.deepface/weights/arcface_weights.h5

# Pre-compute embeddings for all enrolled/ photos and save to pickle.
# This makes startup instant -- no GPU/CPU computation needed at runtime.
COPY precompute_embeddings.py .
RUN python precompute_embeddings.py

ENV PORT=8080
CMD cd gateway && uvicorn app:app --host 0.0.0.0 --port $PORT
