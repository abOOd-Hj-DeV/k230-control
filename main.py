import time
import numpy as np
import threading
from capture.video.client import ScrcpyServerCapture
from capture.video.frame_handler import FrameHandler
from capture.video.frame_saver import FrameSaver
from capture.audio.audio_handler import AudioHandler
from capture.audio.audio_saver import AudioSaver
from capture.sync.sync_manager import SyncManager
from utils.logger import EventLogger, log
from config import (
    ADB_PATH, SERVER_PATH, MAX_FPS, MAX_SIZE,
    CHECK_INTERVAL, SAVE_ALL_FRAMES, FRAME_COUNTER_INTERVAL,
)


def main():
    print("=== STARTING SYNC TEST ===")
    log.info("Starting sync test (Video + Opus Audio + SyncManager)")

    capture = ScrcpyServerCapture(
        adb_path=ADB_PATH,
        server_path=SERVER_PATH,
        max_size=MAX_SIZE,
        max_fps=MAX_FPS,
        audio=True,
        audio_codec="opus",
        audio_bitrate=128_000
    )

    saver = FrameSaver()
    audio_saver = AudioSaver(sample_rate=48000, channels=2, chunk_duration=0.1)
    logger = EventLogger()

    video_handler = FrameHandler(capture)
    audio_handler = AudioHandler(capture, poll_interval=0.01)
    sync_manager = SyncManager(max_buffer_size=100)

    stats = {
        "total_frames": 0,
        "saved_frames": 0,
        "audio_chunks": 0,
        "sync_events": 0,
        "last_time": time.time(),
        "fps_count": 0
    }

    def process_frame(frame: np.ndarray, ts: float):
        stats["total_frames"] += 1
        stats["fps_count"] += 1
        sync_manager.add_video_frame(frame, pts=None, wall_time=ts)

        if ts - stats["last_time"] >= 1.0:
            fps = stats["fps_count"] / (ts - stats["last_time"])
            print(f"[Video FPS: {fps:.1f}] total={stats['total_frames']}")
            stats["fps_count"] = 0
            stats["last_time"] = ts

        if SAVE_ALL_FRAMES and stats["total_frames"] % FRAME_COUNTER_INTERVAL == 0:
            saver.save(frame, label=f"TEST_{stats['total_frames']:06d}")
            stats["saved_frames"] += 1

    def process_audio(audio_chunk: np.ndarray, pts: int | None, ts: float):
        stats["audio_chunks"] += 1
        audio_saver.save(audio_chunk)
        sync_manager.add_audio_chunk(audio_chunk, pts=pts, wall_time=ts)

        if stats["audio_chunks"] % 50 == 0:
            audio_stats = audio_saver.get_stats()
            print(f"[Audio] {stats['audio_chunks']} chunks | "
                  f"{audio_stats['total_files_saved']} files | "
                  f"Buffer: {audio_stats['buffer_duration_s']:.2f}s")

    def monitor_sync():
        while video_handler.is_running():
            synced = sync_manager.get_synced_pair(tolerance_ms=50.0)
            if synced:
                stats["sync_events"] += 1
                if stats["sync_events"] % 5 == 0:
                    sync_stats = sync_manager.get_stats()
                    print(f"[Sync] Event #{stats['sync_events']} | "
                          f"Avg error: {sync_stats['avg_sync_error_ms']:.2f}ms")
            time.sleep(0.1)

    video_handler.on_frame(process_frame)
    audio_handler.on_audio(process_audio)
    monitor_thread = threading.Thread(target=monitor_sync, daemon=True)

    try:
        print("=== CONNECTING ===")
        capture.start()

        if not capture.is_running():
            print("=== CAPTURE FAILED ===")
            return

        print("=== STARTING STREAMS ===")
        monitor_thread.start()
        video_handler.start()
        audio_handler.start()

        print("=== STREAMING (Video + Opus Audio + Sync) ===")
        print("Press Ctrl+C to stop...")
        print("-" * 60)

        while video_handler.is_running():
            time.sleep(0.1)

    except KeyboardInterrupt:
        print("\n=== STOPPING ===")
    except Exception as e:
        print(f"=== ERROR: {e} ===")
        import traceback
        traceback.print_exc()
    finally:
        print("=== CLEANING UP ===")
        video_handler.stop()
        audio_handler.stop()
        capture.stop()
        saver.stop()
        audio_saver.stop()
        logger.close()

        print("-" * 60)
        print("=== FINAL STATS ===")
        print(f"Frames: {stats['total_frames']}")
        print(f"Saved: {stats['saved_frames']}")
        print(f"Audio chunks: {stats['audio_chunks']}")
        print(f"Sync events: {stats['sync_events']}")
        audio_stats = audio_saver.get_stats()
        print(f"Audio files: {audio_stats['total_files_saved']}")
        print("=== DONE ===")


if __name__ == "__main__":
    main()