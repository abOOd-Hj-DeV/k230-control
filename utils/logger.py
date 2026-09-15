import logging
from pathlib import Path
from datetime import datetime
from typing import Dict, Optional
from config import LOG_LEVEL

logging.basicConfig(
    level=getattr(logging, LOG_LEVEL),
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("monitor")


class EventLogger:
    def __init__(self, log_file: Path = Path("output/events.log")):
        self.log_file = log_file
        self.log_file.parent.mkdir(parents=True, exist_ok=True)
        
        # افتح الملف مرة واحدة فقط
        self._file = open(self.log_file, "a", encoding="utf-8", buffering=1)
        
        # اكتب header
        self._file.write("=" * 80 + "\n")
        self._file.write(f"EVENT LOG - Started at {datetime.now().isoformat()}\n")
        self._file.write("=" * 80 + "\n\n")
        self._file.flush()

    def record(self, label: str, confidence: float,
               scores: Dict, image_path: Optional[Path] = None):
        ts = datetime.now().isoformat()
        
        lines = [
            f"[{ts}] [{label}] confidence={confidence:.4f}",
            f"  scores: {scores}",
        ]
        if image_path:
            lines.append(f"  image: {image_path}")
        lines.append("")
        
        self._file.write("\n".join(lines) + "\n")
        self._file.flush()
        
        log.info(f"[{label}] conf={confidence:.2f} scores={scores} img={image_path}")

    def alert(self, label: str, confidence: float,
              scores: Dict, image_path: Optional[Path] = None):
        ts = datetime.now().isoformat()
        
        lines = [
            "!" * 80,
            f"[ALERT] [{ts}] [{label}] confidence={confidence:.4f}",
            f"  scores: {scores}",
        ]
        if image_path:
            lines.append(f"  image: {image_path}")
        lines.extend(["!" * 80, ""])
        
        self._file.write("\n".join(lines) + "\n")
        self._file.flush()
        
        log.warning(f"ALERT: {label} conf={confidence:.2f}")

    def close(self):
        """أغلق الملف عند إيقاف البرنامج"""
        if self._file and not self._file.closed:
            self._file.write(f"\n=== Closed at {datetime.now().isoformat()} ===\n")
            self._file.close()