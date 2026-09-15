#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "k230/bridge/adb_controller.hpp"
#include "k230/bridge/subprocess.hpp"
#include "k230/bridge/tcp_socket.hpp"
#include "k230/ipc/channel.hpp"
#include "k230/media_packet.hpp"
#include "k230/scrcpy_demuxer.hpp"

namespace k230::bridge {

struct ScrcpyConfig {
  std::string server_jar = "assets/scrcpy-server";  // local path of the server jar
  std::string remote_jar = "/data/local/tmp/scrcpy-server.jar";
  std::string server_version = "4.0";  // must match the jar exactly
  std::string serial;                  // empty = first usable device

  std::uint16_t video_port = 27183;
  std::uint16_t audio_port = 27184;

  std::uint32_t max_size = 800;
  std::uint32_t max_fps = 10;
  std::uint32_t video_bit_rate = 2'000'000;
  std::string video_codec = "h264";  // h264 | h265 (both decodable by K230 VDEC)

  bool audio = true;
  std::string audio_codec = "raw";  // raw = PCM s16le 48kHz stereo, no decoder needed
  std::uint32_t audio_bit_rate = 128'000;

  std::string log_level = "info";
  int connect_attempts = 100;
  int connect_retry_delay_ms = 100;
};

// Drives one scrcpy-server instance on the phone and turns its two media
// sockets into MediaPackets pushed to `sink`. All network I/O happens on two
// internal threads; the caller only sees fully framed packets.
class ScrcpySession {
 public:
  using SessionCallback = std::function<void(const VideoSessionInfo&)>;

  ScrcpySession(ScrcpyConfig config, AdbController adb, std::shared_ptr<ipc::PacketSink> sink);
  ~ScrcpySession();
  ScrcpySession(const ScrcpySession&) = delete;
  ScrcpySession& operator=(const ScrcpySession&) = delete;

  void set_session_callback(SessionCallback cb) { on_session_ = std::move(cb); }

  // Push server, forward ports, start server, connect sockets, start threads.
  bool start();
  // True while both reader threads are alive and the server process is running.
  bool running() const;
  void stop();

  const std::string& device_serial() const { return serial_; }
  std::optional<VideoSessionInfo> video_session() const;
  ScrcpyDemuxer::Stats video_stats() const;
  ScrcpyDemuxer::Stats audio_stats() const;
  std::uint32_t scid() const { return scid_; }

  // Exposed for tests / documentation.
  std::vector<std::string> server_command() const;
  std::string socket_name() const;

 private:
  bool connect_video_socket();
  bool connect_audio_socket();
  void reader_loop(TcpSocket& socket, ScrcpyDemuxer& demuxer, const char* tag);

  ScrcpyConfig config_;
  AdbController adb_;
  std::shared_ptr<ipc::PacketSink> sink_;
  SessionCallback on_session_;

  std::string serial_;
  std::uint32_t scid_ = 0;
  ChildProcess server_;
  TcpSocket video_socket_;
  TcpSocket audio_socket_;

  std::unique_ptr<ScrcpyDemuxer> video_demuxer_;
  std::unique_ptr<ScrcpyDemuxer> audio_demuxer_;
  std::thread video_thread_;
  std::thread audio_thread_;
  std::atomic<bool> stop_{false};
  std::atomic<int> live_readers_{0};

  mutable std::mutex state_mutex_;
  std::optional<VideoSessionInfo> video_session_;
};

}  // namespace k230::bridge
