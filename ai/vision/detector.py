import numpy as np
from typing import Dict
from config import ENABLE_AI


class NSFWDetector:
    """كاشف NSFW - أو Mock عند تعطيل AI"""

    def __init__(self):
        if ENABLE_AI:
            try:
                import opennsfw2 as nsfw
                self._model = nsfw.make_open_nsfw_model()
                self._preprocess = nsfw.preprocess_image
                self._available = True
                print("[NSFWDetector] Model loaded successfully")
            except Exception as e:
                print(f"[NSFWDetector] Failed to load model: {e}")
                self._available = False
        else:
            self._available = False
            print("[NSFWDetector] Test mode (Mock) - AI disabled")

    def analyze(self, frame: np.ndarray) -> Dict[str, float]:
        if not self._available:
            return {"sfw": 1.0, "nsfw": 0.0}

        import cv2
        from PIL import Image
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        img = Image.fromarray(rgb)
        processed = self._preprocess(img)
        processed = np.expand_dims(processed, axis=0)
        preds = self._model.predict(processed, verbose=0)[0]
        return {"sfw": float(preds[0]), "nsfw": float(preds[1])}