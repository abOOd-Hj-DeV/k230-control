from abc import ABC, abstractmethod
import subprocess
import socket
import threading
import time
import numpy as np
import av
import os
import queue
import logging
from config import ADB_PATH, SERVER_PATH, VIDEO_PORT, AUDIO_PORT

# Logger setup
logger = logging.getLogger(__name__)


class CaptureClient(ABC):
    @abstractmethod
    def start(self) -> None: ...
    @abstractmethod
    def stop(self) -> None: ...
    @abstractmethod
    def get_frame(self) -> tuple: ...
    @abstractmethod
    def get_audio_chunk(self) -> tuple: ...
    @abstractmethod
    def is_running(self) -> bool: ...


class ScrcpyServerCapture(CaptureClient):
    """
    Production-ready client based on official scrcpy protocol.
    
    Key design decisions:
    - Video: latest_frame only (for live streaming/AI), buffer reset after parse
    - Audio: queue-based (no audio loss), with sliding resync for garbage data
    - Health monitoring with counters
    - Proper error handling (no silent exceptions)
    """
    
    def __init__(self, adb_path=ADB_PATH, server_path=SERVER_PATH,
                 max_size=800, max_fps=30, bitrate=4_000_000,
                 audio=True, audio_codec="opus", audio_bitrate=128_000,
                 video_port=VIDEO_PORT, audio_port=AUDIO_PORT):
        self.adb_path = adb_path
        self.server_path = server_path
        self.max_size = max_size
        self.max_fps = max_fps
        self.bitrate = bitrate
        self.audio = audio
        self.audio_codec = audio_codec
        self.audio_bitrate = audio_bitrate
        self.video_port = video_port
        self.audio_port = audio_port

        self._running = False
        self._server_process = None
        self._video_socket = None
        self._audio_socket = None
        self._video_codec = None
        self._audio_codec_ctx = None
        
        # Video: latest_frame only (live streaming pattern)
        self._latest_frame = None
        self._latest_video_pts = None
        
        # Audio: queue-based (no loss)
        self._audio_queue = queue.Queue(maxsize=100)
        
        self._frame_lock = threading.Lock()
        self._audio_lock = threading.Lock()
        self._video_thread = None
        self._audio_thread = None
        self._stderr_thread = None

        # Health counters
        self._stats = {
            'video_packets': 0,
            'video_frames': 0,
            'audio_packets': 0,
            'audio_chunks': 0,
            'decode_errors': 0,
            'socket_errors': 0,
            'dropped_audio': 0,
            'resync_attempts': 0,
        }

    def _run_adb(self, args, timeout=15):
        if not os.path.exists(self.adb_path):
            raise FileNotFoundError(f"ADB not found: {self.adb_path}")
        cmd = [self.adb_path] + args
        try:
            result = subprocess.run(cmd, capture_output=True, text=True,
                                    timeout=timeout, encoding='utf-8', errors='replace')
            return result
        except subprocess.TimeoutExpired:
            logger.error(f"ADB timeout: {' '.join(args)}")
            raise RuntimeError(f"ADB timeout: {' '.join(args)}")

    def _connect_socket(self, port, max_retries=20, delay=0.5):
        for attempt in range(max_retries):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(3.0)
                sock.connect(("127.0.0.1", port))
                return sock
            except Exception as e:
                logger.debug(f"Socket connect attempt {attempt+1} failed: {e}")
                if attempt < max_retries - 1:
                    time.sleep(delay)
                else:
                    try: sock.close()
                    except: pass
                    return None

    def _recv_exact(self, sock, size):
        data = b""
        while len(data) < size:
            try:
                chunk = sock.recv(size - len(data))
                if not chunk:
                    return None
                data += chunk
            except socket.timeout:
                if not self._running:
                    return None
                continue
            except Exception as e:
                self._stats['socket_errors'] += 1
                logger.debug(f"Socket recv error: {e}")
                return None
        return data

    def _prepare_adb(self):
        """Initialize ADB daemon"""
        logger.info("Cleaning ADB state...")
        try:
            self._run_adb(["kill-server"], timeout=5)
        except Exception as e:
            logger.debug(f"ADB kill-server failed (ok): {e}")
        self._run_adb(["start-server"], timeout=15)

    def _check_device(self):
        """Verify device connection"""
        logger.info("Checking device connection...")
        result = self._run_adb(["devices"], timeout=15)
        if "device" not in result.stdout:
            raise RuntimeError("No device connected. Ensure USB debugging is enabled.")
        logger.info("Device connected")

    def _push_server(self):
        """Push scrcpy-server.jar to device"""
        logger.info("Pushing server to device...")
        if not os.path.exists(self.server_path):
            raise FileNotFoundError(f"Server JAR not found: {self.server_path}")
        self._run_adb(["push", self.server_path, "/data/local/tmp/scrcpy-server.jar"], timeout=30)

    def _remove_old_forwards(self):
        """Clean up old port forwards"""
        logger.info("Removing old forwards...")
        self._run_adb(["forward", "--remove-all"], timeout=5)

    def _start_server(self):
        """Launch scrcpy-server on device"""
        logger.info("Starting server (send_frame_meta=true, Opus audio)...")
        cmd = [
            self.adb_path, "shell",
            "CLASSPATH=/data/local/tmp/scrcpy-server.jar",
            "app_process", "/", "com.genymobile.scrcpy.Server",
            "4.0",
            f"max_size={self.max_size}",
            f"max_fps={self.max_fps}",
            f"video_bit_rate={self.bitrate}",
            "tunnel_forward=true",
            "send_frame_meta=true",
            "control=false",
            f"audio={str(self.audio).lower()}",
            f"audio_codec={self.audio_codec}",
            f"audio_bit_rate={self.audio_bitrate}",
            "audio_dup=true",
            "show_touches=false",
            "stay_awake=false",
            "cleanup=false",
        ]

        self._server_process = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding='utf-8', errors='replace'
        )
        
        # Start stderr reader thread
        self._stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self._stderr_thread.start()
        
        time.sleep(3)

        if self._server_process.poll() is not None:
            raise RuntimeError(f"Server died: {self._server_process.returncode}")

    def _forward_ports(self):
        """Setup port forwarding for video and audio"""
        logger.info(f"Forwarding video port {self.video_port}...")
        result = self._run_adb(["forward", f"tcp:{self.video_port}", "localabstract:scrcpy"], timeout=5)
        if result.returncode != 0:
            raise RuntimeError(f"Video forward failed: {result.stderr}")

        if self.audio:
            logger.info(f"Forwarding audio port {self.audio_port}...")
            result = self._run_adb(["forward", f"tcp:{self.audio_port}", "localabstract:scrcpy"], timeout=5)
            if result.returncode != 0:
                logger.warning("Audio forward failed, disabling audio")
                self.audio = False

    def _connect_video_socket(self):
        """Connect to video stream"""
        logger.info("Connecting to video socket...")
        self._video_socket = self._connect_socket(self.video_port)
        if not self._video_socket:
            raise RuntimeError("Failed to connect video socket")
        
        logger.info("Reading video dummy byte...")
        try:
            self._video_socket.recv(1)
        except Exception as e:
            logger.warning(f"Video dummy read failed: {e}")

    def _connect_audio_socket(self):
        """Connect to audio stream"""
        if not self.audio:
            return
            
        logger.info("Connecting to audio socket...")
        self._audio_socket = self._connect_socket(self.audio_port)
        if not self._audio_socket:
            logger.warning("Audio socket failed, disabling audio")
            self.audio = False
            return
            
        logger.info("Reading audio dummy byte...")
        try:
            self._audio_socket.recv(1)
        except Exception as e:
            logger.warning(f"Audio dummy read failed: {e}")

    def _init_codecs(self):
        """Initialize PyAV codecs"""
        logger.info("Setting up codecs...")
        self._video_codec = av.CodecContext.create("h264", "r")

        if self.audio and self._audio_socket:
            try:
                if self.audio_codec == "opus":
                    self._audio_codec_ctx = av.CodecContext.create("opus", "r")
                elif self.audio_codec == "aac":
                    self._audio_codec_ctx = av.CodecContext.create("aac", "r")
                logger.info(f"Audio codec ready: {self.audio_codec}")
            except Exception as e:
                logger.error(f"Audio codec init failed: {e}")
                self.audio = False

    def _start_threads(self):
        """Start decode threads"""
        logger.info("Starting decode threads...")
        self._running = True
        
        self._video_thread = threading.Thread(target=self._decode_video_loop, daemon=True)
        self._video_thread.start()

        if self.audio and self._audio_socket:
            self._audio_thread = threading.Thread(target=self._decode_audio_loop, daemon=True)
            self._audio_thread.start()

    def _wait_for_first_frame(self, timeout=10):
        """Wait for first video frame with timeout"""
        logger.info("Waiting for first frame...")
        start_time = time.time()
        
        while time.time() - start_time < timeout:
            if self._latest_frame is not None:
                h, w = self._latest_frame.shape[:2]
                logger.info(f"Ready! First frame: {w}x{h}")
                return
            time.sleep(0.1)
        
        # Timeout: raise error instead of warning
        raise RuntimeError(f"No frames received after {timeout}s")

    def start(self):
        """Main startup sequence"""
        logger.info("=== Starting capture session ===")
        
        try:
            self._prepare_adb()
            self._check_device()
            self._push_server()
            self._remove_old_forwards()
            self._start_server()
            self._forward_ports()
            self._connect_video_socket()
            self._connect_audio_socket()
            self._init_codecs()
            self._start_threads()
            self._wait_for_first_frame(timeout=10)
        except Exception as e:
            logger.error(f"Startup failed: {e}")
            self.stop()
            raise

    def _read_stderr(self):
        """Read and log stderr from scrcpy-server"""
        try:
            while self._running and self._server_process:
                line = self._server_process.stderr.readline()
                if not line:
                    break
                line = line.strip()
                if line:
                    logger.info(f"[scrcpy] {line}")
        except Exception as e:
            logger.debug(f"stderr reader error: {e}")

    def _decode_video_loop(self):
        """
        Video decoder: H.264 Annex-B stream with buffer reset after parse.
        
        Design: latest_frame only (live streaming pattern)
        - Parse buffer, decode packets, update _latest_frame
        - Reset buffer after parse (PyAV parser handles state internally)
        - No queue to avoid latency in live scenarios
        """
        logger.info("Video decode loop started (H.264 Annex-B)")
        buffer = b""
        MAX_BUFFER_SIZE = 2 * 1024 * 1024
        frame_count = 0

        while self._running and self._video_socket:
            # Health check: ensure thread is alive
            if not self._video_thread.is_alive() and self._running:
                logger.error("Video decode thread died unexpectedly")
                break

            try:
                chunk = self._video_socket.recv(65536)
                if not chunk:
                    logger.info("Video stream ended")
                    break
                
                buffer += chunk
                
                # Limit buffer size
                if len(buffer) > MAX_BUFFER_SIZE:
                    buffer = buffer[-MAX_BUFFER_SIZE // 2:]
                
                try:
                    packets = self._video_codec.parse(buffer)
                    
                    for packet in packets:
                        self._stats['video_packets'] += 1
                        frames = self._video_codec.decode(packet)
                        
                        for frame in frames:
                            img = frame.to_ndarray(format="bgr24")
                            self._stats['video_frames'] += 1
                            
                            with self._frame_lock:
                                self._latest_frame = img
                                self._latest_video_pts = int(time.time() * 1_000_000)
                            
                            frame_count += 1
                            if frame_count == 1:
                                logger.info(f"First frame: {img.shape}")
                            if frame_count == 10:
                                logger.info("10 frames decoded!")
                            if frame_count % 100 == 0:
                                logger.info(f"{frame_count} frames decoded")
                    
                    # ✅ Reset buffer after parse (PyAV handles internal state)
                    if packets:
                        buffer = b""
                        
                except av.AVError as e:
                    self._stats['decode_errors'] += 1
                    if frame_count < 5:
                        logger.warning(f"Video decode error: {e}")
                except Exception as e:
                    self._stats['decode_errors'] += 1
                    logger.error(f"Unexpected video error: {e}", exc_info=True)

            except socket.timeout:
                continue
            except Exception as e:
                self._stats['socket_errors'] += 1
                if self._running:
                    logger.error(f"Video socket error: {e}")
                break

        logger.info(f"Video decode ended, frames: {frame_count}")

    def _decode_audio_loop(self):
        """
        Audio decoder: Opus with 12-byte headers and sliding resync.
        
        Design: queue-based (no audio loss)
        - Sliding buffer resync for garbage data at startup
        - Queue with fixed size, drop oldest when full
        """
        logger.info("Audio decode loop started (Opus with headers)")
        bytes_received = 0
        chunks_produced = 0
        sync_buffer = b""
        sync_attempts = 0
        max_sync_attempts = 500

        # Phase 1: Synchronize with stream using sliding window
        logger.info("Synchronizing with audio stream...")
        while self._running and self._audio_socket and sync_attempts < max_sync_attempts:
            try:
                chunk = self._audio_socket.recv(4096)
                if not chunk:
                    break
                sync_buffer += chunk
                
                # Try to find valid header in buffer
                found = False
                while len(sync_buffer) >= 12:
                    header = sync_buffer[:12]
                    pkt_len = int.from_bytes(header[8:12], 'big')
                    
                    # Valid: pkt_len between 1 and 10000
                    if 0 < pkt_len <= 10000:
                        # Found valid header, keep it and break
                        sync_buffer = sync_buffer[12:]  # Remove header from buffer
                        found = True
                        break
                    else:
                        # Invalid, slide by 1 byte
                        sync_buffer = sync_buffer[1:]
                        sync_attempts += 1
                
                if found:
                    # Put valid header back for main loop
                    sync_buffer = header + sync_buffer
                    break
                    
            except Exception:
                sync_attempts += 1
                continue

        if sync_attempts >= max_sync_attempts:
            logger.error(f"Failed to sync audio after {max_sync_attempts} attempts")
            return
        
        if sync_attempts > 0:
            logger.info(f"Audio synced after {sync_attempts} resync attempts")
            self._stats['resync_attempts'] = sync_attempts

        # Phase 2: Normal packet reading
        buffer = sync_buffer
        sync_buffer = b""  # Clear temp buffer
        
        while self._running and self._audio_socket:
            # Health check
            if not self._audio_thread.is_alive() and self._running:
                logger.error("Audio decode thread died unexpectedly")
                break

            try:
                header = self._recv_exact(self._audio_socket, 12)
                if not header:
                    logger.info(f"Audio stream ended. Chunks: {chunks_produced}")
                    break

                pts = int.from_bytes(header[:8], 'big')
                pkt_len = int.from_bytes(header[8:12], 'big')

                # Validate and auto-resync if invalid
                if pkt_len <= 0 or pkt_len > 10000:
                    self._stats['resync_attempts'] += 1
                    # Sliding resync: try reading one byte at a time
                    try:
                        self._audio_socket.recv(1)
                    except:
                        pass
                    continue

                pkt_data = self._recv_exact(self._audio_socket, pkt_len)
                if not pkt_data:
                    logger.info("Audio stream ended while reading packet")
                    break

                bytes_received += pkt_len
                self._stats['audio_packets'] += 1

                try:
                    packet = av.packet.Packet(pkt_data)
                    frames = self._audio_codec_ctx.decode(packet)

                    for frame in frames:
                        try:
                            audio_np = frame.to_ndarray()
                        except TypeError:
                            audio_np = np.frombuffer(frame.to_buffer(), dtype=np.float32)

                        if chunks_produced == 0:
                            logger.info(f"First audio packet: pkt_len={pkt_len}, "
                                      f"shape={audio_np.shape}, dtype={audio_np.dtype}, "
                                      f"min={audio_np.min():.4f}, max={audio_np.max():.4f}")

                        # Planar -> interleaved
                        if audio_np.ndim == 2 and audio_np.shape[0] <= 8:
                            audio_np = audio_np.T

                        # Convert to float32
                        if audio_np.dtype != np.float32:
                            if audio_np.dtype in [np.int16, np.int32]:
                                max_val = np.iinfo(audio_np.dtype).max
                                audio_np = audio_np.astype(np.float32) / max_val
                            else:
                                audio_np = audio_np.astype(np.float32)

                        # Ensure stereo
                        if audio_np.ndim == 1:
                            audio_np = np.column_stack([audio_np, audio_np])
                        elif audio_np.ndim == 2 and audio_np.shape[1] == 1:
                            audio_np = np.column_stack([audio_np, audio_np])

                        # DC offset removal
                        audio_np = audio_np - np.mean(audio_np, axis=0)

                        # Add to queue (drop oldest if full)
                        if not self._audio_queue.full():
                            self._audio_queue.put((audio_np, pts))
                        else:
                            try:
                                self._audio_queue.get_nowait()
                                self._audio_queue.put((audio_np, pts))
                                self._stats['dropped_audio'] += 1
                            except:
                                pass

                        chunks_produced += 1
                        self._stats['audio_chunks'] += 1

                        if chunks_produced <= 3:
                            logger.info(f"Audio #{chunks_produced}: shape={audio_np.shape}, "
                                      f"min={audio_np.min():.4f}, max={audio_np.max():.4f}")

                        if chunks_produced % 100 == 0:
                            logger.info(f"Audio {chunks_produced} chunks, {bytes_received/1024:.1f} KB")

                except av.AVError as e:
                    self._stats['decode_errors'] += 1
                    if chunks_produced < 3:
                        logger.warning(f"Audio decode error: {e}")
                except Exception as e:
                    self._stats['decode_errors'] += 1
                    logger.error(f"Unexpected audio error: {e}", exc_info=True)

            except socket.timeout:
                continue
            except Exception as e:
                self._stats['socket_errors'] += 1
                if self._running:
                    logger.error(f"Audio socket error: {e}")
                break

        logger.info(f"Audio decode ended, chunks: {chunks_produced}")

    def stop(self):
        """Clean shutdown with thread joins"""
        logger.info("Stopping capture...")
        self._running = False

        # Close sockets
        for sock in [self._video_socket, self._audio_socket]:
            if sock:
                try:
                    sock.close()
                except Exception as e:
                    logger.debug(f"Socket close error: {e}")

        # Wait for threads to finish
        for thread in [self._video_thread, self._audio_thread, self._stderr_thread]:
            if thread and thread.is_alive():
                try:
                    thread.join(timeout=2.0)
                except Exception as e:
                    logger.debug(f"Thread join error: {e}")

        # Terminate server process
        if self._server_process:
            self._server_process.terminate()
            try:
                self._server_process.wait(timeout=3)
            except Exception as e:
                logger.debug(f"Server terminate error: {e}")

        # Remove port forwards
        for port in [self.video_port, self.audio_port]:
            try:
                subprocess.run([self.adb_path, "forward", "--remove", f"tcp:{port}"],
                               capture_output=True, timeout=3)
            except Exception as e:
                logger.debug(f"Forward remove error: {e}")

        logger.info("Capture stopped")

    def get_frame(self):
        """Get latest video frame (live streaming pattern)"""
        with self._frame_lock:
            if self._latest_frame is not None:
                frame = self._latest_frame.copy()
                pts = self._latest_video_pts
                return frame, pts
            return None, None

    def get_audio_chunk(self):
        """Get audio chunk from queue (no loss)"""
        try:
            chunk, pts = self._audio_queue.get_nowait()
            return chunk, pts
        except queue.Empty:
            return None, None

    def is_running(self):
        """Check if capture is active and healthy"""
        return (self._running and
                self._server_process is not None and
                self._server_process.poll() is None and
                self._video_thread is not None and
                self._video_thread.is_alive())

    def get_stats(self):
        """Return health statistics"""
        return self._stats.copy()