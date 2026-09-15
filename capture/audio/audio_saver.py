import wave
import numpy as np
from pathlib import Path
from datetime import datetime
from threading import Lock
from config import OUTPUT_DIR


class AudioSaver:
    """
    يحفظ كل chunk_duration (افتراضياً 0.5 ثانية) من الصوت في ملف WAV منفصل
    مناسب للتحليل اللحظي بالذكاء الاصطناعي
    """
    
    def __init__(self, output_dir: Path = OUTPUT_DIR,
                 sample_rate: int = 48000, channels: int = 2,
                 chunk_duration: float = 0.5):
        self.output_dir = output_dir / "audio"
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.sample_rate = sample_rate
        self.channels = channels
        self.chunk_duration = chunk_duration
        self.samples_per_chunk = int(sample_rate * chunk_duration)
        
        # Buffer لتجميع العينات
        self._buffer = np.zeros((0, channels), dtype=np.float32)
        self._chunk_counter = 0
        self._total_chunks_saved = 0
        self._lock = Lock()
        
        print(f"[AudioSaver] Saving {chunk_duration}s chunks to {self.output_dir}")
        print(f"[AudioSaver] Config: {sample_rate}Hz, {channels}ch, "
              f"{self.samples_per_chunk} samples/chunk")

    def save(self, audio_chunk: np.ndarray):
        """
        أضف chunk صوتي للـ buffer، واحفظ تلقائياً كل chunk_duration ثانية
        """
        if audio_chunk is None or audio_chunk.size == 0:
            return
        
        with self._lock:
            # Debug أول chunk
            if self._total_chunks_saved == 0 and self._chunk_counter == 0:
                print(f"[AudioSaver] First audio chunk received!")
                print(f"  - Shape: {audio_chunk.shape}")
                print(f"  - Dtype: {audio_chunk.dtype}")
                print(f"  - Min: {audio_chunk.min():.4f}, Max: {audio_chunk.max():.4f}")
                print(f"  - Samples: {len(audio_chunk)}")
            
            # تأكد من shape صحيح: (samples, channels)
            audio_chunk = self._normalize_shape(audio_chunk)
            
            # أضف للـ buffer
            self._buffer = np.vstack([self._buffer, audio_chunk])
            self._chunk_counter += 1
            
            # إحصائيات كل 100 chunk
            if self._chunk_counter % 100 == 0:
                buffer_duration = len(self._buffer) / self.sample_rate
                print(f"[AudioSaver] Received {self._chunk_counter} chunks, "
                      f"buffer: {buffer_duration:.2f}s, saved: {self._total_chunks_saved}")
            
            # احفظ كل chunk كامل (0.5s)
            while len(self._buffer) >= self.samples_per_chunk:
                chunk_to_save = self._buffer[:self.samples_per_chunk]
                self._buffer = self._buffer[self.samples_per_chunk:]
                self._save_chunk_to_file(chunk_to_save)

    def _normalize_shape(self, audio_chunk: np.ndarray) -> np.ndarray:
        """تأكد من أن الـ shape هو (samples, channels)"""
        if audio_chunk.ndim == 1:
            # mono → stereo
            return np.column_stack([audio_chunk, audio_chunk])
        elif audio_chunk.ndim == 2:
            if audio_chunk.shape[1] != self.channels:
                if audio_chunk.shape[0] == self.channels:
                    # (channels, samples) → (samples, channels)
                    return audio_chunk.T
                else:
                    # خذ أول channels
                    return audio_chunk[:, :self.channels]
        return audio_chunk

    def _save_chunk_to_file(self, chunk: np.ndarray):
        """حفظ chunk واحد كملف WAV مستقل مع معالجة"""
        ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        filename = f"chunk_{self._total_chunks_saved:06d}_{ts}.wav"
        path = self.output_dir / filename
        
        try:
            # ✅ DC Offset Removal (إزالة الإزاحة)
            chunk = chunk - np.mean(chunk, axis=0)
            
            # ✅ Normalization (تطبيع الصوت)
            max_val = np.max(np.abs(chunk))
            if max_val > 0:
                chunk = chunk / max_val * 0.9  # 90% من الحد الأقصى
            
            # ✅ High-pass filter بسيط (إزالة الضجيج منخفض التردد)
            if len(chunk) > 10:
                window_size = min(100, len(chunk) // 10)
                if window_size > 0:
                    kernel = np.ones(window_size) / window_size
                    for ch in range(self.channels):
                        low_freq = np.convolve(chunk[:, ch], kernel, mode='same')
                        chunk[:, ch] = chunk[:, ch] - low_freq
            
            # clip و convert إلى int16
            chunk = np.clip(chunk, -1.0, 1.0)
            audio_int16 = (chunk * 32767).astype(np.int16)
            
            # كتابة ملف WAV
            with wave.open(str(path), 'wb') as wf:
                wf.setnchannels(self.channels)
                wf.setsampwidth(2)  # 16-bit
                wf.setframerate(self.sample_rate)
                wf.writeframes(audio_int16.tobytes())
            
            self._total_chunks_saved += 1
            
            # طباعة كل 10 ملفات
            if self._total_chunks_saved % 10 == 0:
                print(f"[AudioSaver] [OK] Saved {self._total_chunks_saved} files")
                
        except Exception as e:
            print(f"[AudioSaver] Save error: {e}")

    def get_stats(self) -> dict:
        with self._lock:
            return {
                'total_chunks_received': self._chunk_counter,
                'total_files_saved': self._total_chunks_saved,
                'buffer_samples': len(self._buffer),
                'buffer_duration_s': len(self._buffer) / self.sample_rate
            }

    def stop(self):
        """أوقف واحفظ ما تبقى في الـ buffer"""
        with self._lock:
            # احفظ الباقي إذا كان على الأقل نصف chunk
            if len(self._buffer) >= self.samples_per_chunk // 2:
                # padding بالأصفار ليصبح chunk_duration كاملة
                padding = np.zeros(
                    (self.samples_per_chunk - len(self._buffer), self.channels),
                    dtype=np.float32
                )
                chunk_to_save = np.vstack([self._buffer, padding])
                self._save_chunk_to_file(chunk_to_save)
                self._buffer = np.zeros((0, self.channels), dtype=np.float32)
            
            stats = self.get_stats()
            print(f"[AudioSaver] Stopped - Stats: {stats}")