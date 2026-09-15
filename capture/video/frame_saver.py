import cv2
import numpy as np
from pathlib import Path
from datetime import datetime
from queue import Queue, Empty
from threading import Thread
from config import OUTPUT_DIR


class FrameSaver:
    def __init__(self, output_dir: Path = OUTPUT_DIR):
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self._queue = Queue(maxsize=500)
        self._running = True
        self._thread = Thread(target=self._save_loop, daemon=True)
        self._thread.start()

    def save(self, frame: np.ndarray, label: str = "frame") -> Path:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        filename = f"{label}_{ts}.jpg"
        path = self.output_dir / filename
        self._queue.put((frame, path))
        return path

    def _save_loop(self):
        while self._running or not self._queue.empty():
            try:
                frame, path = self._queue.get(timeout=0.1)
                cv2.imwrite(str(path), frame)
            except Empty:
                continue
            except Exception as e:
                # ✅ معالجة أخطاء واضحة بدلاً من except: pass
                print(f"[FrameSaver] Save failed: {e}")

    def stop(self):
        self._running = False
        if self._thread.is_alive():
            self._thread.join(timeout=2)