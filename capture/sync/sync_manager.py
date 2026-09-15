import time
from collections import deque
from threading import Lock


class SyncManager:
    """
    مدير التزامن بين الفيديو والصوت
    يستخدم PTS (Presentation Timestamp) من السيرفر
    """
    
    def __init__(self, max_buffer_size: int = 100):
        self.video_buffer = deque(maxlen=max_buffer_size)
        self.audio_buffer = deque(maxlen=max_buffer_size)
        self._lock = Lock()
        
        # إحصائيات
        self.sync_stats = {
            'video_frames': 0,
            'audio_chunks': 0,
            'avg_sync_error_ms': 0.0,
            'last_sync_time': 0.0
        }

    def add_video_frame(self, frame, pts: int | None, wall_time: float):
        """أضف frame فيديو للـ buffer"""
        with self._lock:
            self.video_buffer.append({
                'frame': frame,
                'pts': pts,
                'wall_time': wall_time
            })
            self.sync_stats['video_frames'] += 1

    def add_audio_chunk(self, audio_chunk, pts: int | None, wall_time: float):
        """أضف chunk صوتي للـ buffer"""
        with self._lock:
            self.audio_buffer.append({
                'audio_chunk': audio_chunk,
                'pts': pts,
                'wall_time': wall_time
            })
            self.sync_stats['audio_chunks'] += 1

    def get_synced_pair(self, tolerance_ms: float = 50.0) -> tuple | None:
        """
        ابحث عن زوج (video, audio) متزامن
        tolerance_ms = الفرق المسموح بين PTS
        """
        with self._lock:
            if not self.video_buffer or not self.audio_buffer:
                return None
            
            # خذ أحدث frame وأحدث audio
            video = self.video_buffer[-1]
            audio = self.audio_buffer[-1]
            
            if video['pts'] is None or audio['pts'] is None:
                # لا يوجد PTS، استخدم wall_time
                time_diff = abs(video['wall_time'] - audio['wall_time']) * 1000
            else:
                # استخدم PTS (scrcpy يستخدم microseconds)
                time_diff = abs(video['pts'] - audio['pts']) / 1000.0
            
            if time_diff <= tolerance_ms:
                # متزامن!
                self.sync_stats['avg_sync_error_ms'] = time_diff
                self.sync_stats['last_sync_time'] = time.time()
                
                # أزل من الـ buffers
                self.video_buffer.clear()
                self.audio_buffer.clear()
                
                return (video['frame'], audio['audio_chunk'])
            
            # غير متزامن، أحذف الأقدم
            if video['wall_time'] < audio['wall_time']:
                self.video_buffer.popleft()
            else:
                self.audio_buffer.popleft()
            
            return None

    def get_stats(self) -> dict:
        """احصل على إحصائيات التزامن"""
        with self._lock:
            return self.sync_stats.copy()