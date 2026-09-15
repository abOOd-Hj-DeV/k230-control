import os
from pathlib import Path

# Paths
BASE_DIR = Path(__file__).parent
SCRCPY_DIR = BASE_DIR / "scrcpy"
OUTPUT_DIR = BASE_DIR / "output" / "frames"

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

# ADB
ADB_PATH = str(SCRCPY_DIR / "adb.exe")
SERVER_PATH = str(SCRCPY_DIR / "scrcpy-server")

# scrcpy settings
MAX_FPS = 10
MAX_SIZE = 800
BITRATE = 2_000_000

# AI settings
NSFW_THRESHOLD = 0.75
CHECK_INTERVAL = 0.1   # 10 fps

# Logging
LOG_LEVEL = "INFO"

# Test mode
ENABLE_AI = False
SAVE_ALL_FRAMES = True
FRAME_COUNTER_INTERVAL = 1  # احفظ كل frame
# ... existing config ...

# Capture ports
VIDEO_PORT = 27183
AUDIO_PORT = 27184