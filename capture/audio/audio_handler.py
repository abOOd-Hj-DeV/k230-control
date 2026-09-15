import time
import numpy as np
from typing import Callable
from threading import Thread, Event
# يجب أن يكون السطر 5 هكذا:
from capture.video.client import CaptureClient  # ✅ ليس from .client

class AudioHandler:
    """معالج الصوت - مشابه لـ FrameHandler"""
    
    def __init__(self, capture: CaptureClient, poll_interval: float = 0.01):
        self.capture = capture
        self.poll_interval = poll_interval
        self._callbacks = []
        self._stop_event = Event()
        self._thread = None

    def on_audio(self, callback: Callable):
        """سجل callback لمعالجة الصوت"""
        self._callbacks.append(callback)

    def start(self):
        self._stop_event.clear()
        self._thread = Thread(target=self._loop, daemon=True)
        self._thread.start()
        print("[AudioHandler] Started")

    def _loop(self):
        while not self._stop_event.wait(self.poll_interval):
            if not self.capture.is_running():
                break
            
            audio_chunk, pts = self.capture.get_audio_chunk()
            
            if audio_chunk is None:
                continue

            now = time.time()
            for cb in self._callbacks:
                try:
                    cb(audio_chunk, pts, now)
                except Exception as e:
                    print(f"[AudioHandler] Callback error: {e}")

    def stop(self):
        self._stop_event.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1)
        print("[AudioHandler] Stopped")

    def is_running(self) -> bool:
        return not self._stop_event.is_set()