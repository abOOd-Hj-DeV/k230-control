import time
import numpy as np
from typing import Callable
from threading import Thread, Event
from capture.video.client import CaptureClient
from config import CHECK_INTERVAL


class FrameHandler:
    def __init__(self, capture: CaptureClient):
        self.capture = capture
        self.interval = CHECK_INTERVAL
        self._callbacks = []
        self._stop_event = Event()
        self._thread = None

    def on_frame(self, callback: Callable):
        self._callbacks.append(callback)

    def start(self):
        self._stop_event.clear()
        self._thread = Thread(target=self._loop, daemon=True)
        self._thread.start()

    def _loop(self):
        # استخدام Event بدلاً من busy waiting
        while not self._stop_event.wait(self.interval):
            if not self.capture.is_running():
                break
                
            frame, pts = self.capture.get_frame()
            if frame is None:
                continue

            now = time.time()
            for cb in self._callbacks:
                try:
                    cb(frame, now)
                except Exception as e:
                    print(f"[FrameHandler] Callback error: {e}")

    def stop(self):
        self._stop_event.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1)

    def is_running(self) -> bool:
        return not self._stop_event.is_set()