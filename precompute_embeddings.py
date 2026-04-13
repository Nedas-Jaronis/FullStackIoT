import pickle
import numpy as np
from pathlib import Path
from deepface import DeepFace

enrolled = Path('enrolled')
cache = []
if enrolled.exists():
    for p in sorted(enrolled.iterdir()):
        if not p.is_dir():
            continue
        for img in sorted(p.glob('*.jpg')):
            try:
                reps = DeepFace.represent(str(img), model_name='ArcFace', enforce_detection=True)
                if reps:
                    cache.append({'name': p.name, 'embedding': reps[0]['embedding'], 'source': f'{p.name}/{img.name}'})
                    print(f'OK {p.name}/{img.name}')
            except Exception as e:
                print(f'SKIP {img.name}: {e}')

print(f'Pre-computed {len(cache)} embeddings')
with open('enrolled_embeddings.pkl', 'wb') as f:
    pickle.dump(cache, f)
