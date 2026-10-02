#include "k230/bridge/scrcpy_session.hpp"

#include <chrono>
#include <cstdio>
#include <random>

#include "k230/log.hpp"

namespace k230::bridge {

namespace {
constexpr const char* kTag = "scrcpy";
constexpr std::size_t kRecvChunk = 64 * 1024;
}  // namespace

ScrcpySession::ScrcpySession(ScrcpyConfig config, AdbController adb, std::shared_ptr<ipc::PacketSink> sink)
    : config_(std::move(config)), adb_(std::move(adb)), sink_(std::move(sink)) {
  std::random_device rd;
  scid_ = rd() & 0x7fffffffu;
}

ScrcpySession::~ScrcpySession() { stop(); }

std::string ScrcpySession::scid_hex() const {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", scid_);
  return buf;
}

std::string ScrcpySession::socket_name() const { return "scrcpy_" + scid_hex(); }

std::vector<std::string> ScrcpySession::server_command() const {
  // Mirrors what the official client sends (see scrcpy app/src/server.c).
  return {
      "CLASSPATH=" + config_.remote_jar,
      "app_process",
      "/",
      "com.genymobile.scrcpy.Server",
      config_.server_version,
      "scid=" + scid_hex(),
      "log_level=" + config_.log_level,
      "video=true",
      "video_codec=" + config_.video_codec,
      "max_size=" + std::to_string(config_.max_size),
      "max_fps=" + std::to_string(config_.max_fps),
      "video_bit_rate=" + std::to_string(config_.video_bit_rate),
      std::string("audio=") + (config_.audio ? "true" : "false"),
      "audio_source=playback",
      "audio_dup=true",
      "audio_codec=" + config_.audio_codec,
      "audio_bit_rate=" + std::to_string(config_.audio_bit_rate),
      "control=false",
      "tunnel_forward=true",
      "send_device_meta=false",
      "send_frame_meta=true",
      "send_dummy_byte=true",
      "cleanup=true",
  };
}

bool ScrcpySession::start() {
  auto device = adb_.pick_device(config_.serial);
  if (!device) {
    K230_LOG_ERROR(kTag) << "no usable adb device (is USB debugging enabled and authorised?)";
    return false;
  }
  serial_ = device->serial;
  K230_LOG_INFO(kTag) << "using device " << serial_;

  if (!adb_.push(serial_, config_.server_jar, config_.remote_jar)) return false;

  const std::string remote = "localabstract:" + socket_name();
  if (!adb_.forward(serial_, config_.video_port, remote)) return false;
  if (config_.audio && !adb_.forward(serial_, config_.audio_port, remote)) return false;

  if (!adb_.shell_async(serial_, server_command(), server_)) {
    K230_LOG_ERROR(kTag) << "failed to launch scrcpy-server";
    return false;
  }

  if (!connect_video_socket()) {
    for (const auto& line : server_.last_lines()) K230_LOG_ERROR("scrcpy-server") << line;
    stop();
    return false;
  }
  if (config_.audio && !connect_audio_socket()) {
    stop();
    return false;
  }

  ScrcpyDemuxer::Callbacks video_cb;
  video_cb.on_codec = [](CodecId id) { K230_LOG_INFO(kTag) << "video codec: " << to_string(id); };
  video_cb.on_session = [this](const VideoSessionInfo& info) {
    K230_LOG_INFO(kTag) << "video session " << info.width << "x" << info.height
                        << (info.client_resized ? " (client resized)" : "");
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      video_session_ = info;
    }
    if (on_session_) on_session_(info);
  };
  video_cb.on_packet = [this](MediaPacket&& p) { sink_->push(std::move(p)); };
  ScrcpyDemuxer::Options video_opts;
  video_opts.stream = StreamType::Video;
  video_demuxer_ = std::make_unique<ScrcpyDemuxer>(video_opts, std::move(video_cb));

  if (config_.audio) {
    ScrcpyDemuxer::Callbacks audio_cb;
    audio_cb.on_codec = [](CodecId id) { K230_LOG_INFO(kTag) << "audio codec: " << to_string(id); };
    audio_cb.on_packet = [this](MediaPacket&& p) { sink_->push(std::move(p)); };
    ScrcpyDemuxer::Options audio_opts;
    audio_opts.stream = StreamType::Audio;
    audio_demuxer_ = std::make_unique<ScrcpyDemuxer>(audio_opts, std::move(audio_cb));
  }

  stop_ = false;
  live_readers_ = config_.audio ? 2 : 1;
  video_thread_ = std::thread([this] { reader_loop(video_socket_, *video_demuxer_, "video"); });
  if (config_.audio) {
    audio_thread_ = std::thread([this] { reader_loop(audio_socket_, *audio_demuxer_, "audio"); });
  }
  return true;
}

bool ScrcpySession::connect_video_socket() {
  // With tunnel_forward the adb daemon accepts our TCP connection even before
  // the server is listening, then closes it. The server therefore sends one
  // dummy byte as soon as it accepts; a connection on which we cannot read
  // that byte is stale and must be retried.
  for (int attempt = 0; attempt < config_.connect_attempts; ++attempt) {
    if (!server_.running()) {
      K230_LOG_ERROR(kTag) << "scrcpy-server exited before accepting connections";
      return false;
    }
    if (video_socket_.connect("127.0.0.1", config_.video_port)) {
      std::uint8_t dummy = 0;
      if (video_socket_.recv_exact(&dummy, 1, 500)) {
        K230_LOG_INFO(kTag) << "video socket connected after " << attempt + 1 << " attempt(s)";
        return true;
      }
      video_socket_.close();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.connect_retry_delay_ms));
  }
  K230_LOG_ERROR(kTag) << "timed out connecting to the video socket";
  return false;
}

bool ScrcpySession::connect_audio_socket() {
  // The server accepts video then audio back-to-back, so this succeeds
  // immediately once the video socket is up.
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (audio_socket_.connect("127.0.0.1", config_.audio_port)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.connect_retry_delay_ms));
  }
  K230_LOG_ERROR(kTag) << "cannot connect to the audio socket";
  return false;
}

void ScrcpySession::reader_loop(TcpSocket& socket, ScrcpyDemuxer& demuxer, const char* tag) {
  std::vector<std::uint8_t> buf(kRecvChunk);
  while (!stop_) {
    long n = socket.recv(buf.data(), buf.size(), 1000);
    if (n == 0) {
      K230_LOG_WARN(kTag) << tag << " socket closed by server";
      break;
    }
    if (n < 0) continue;  // timeout, re-check stop flag
    if (!demuxer.feed(buf.data(), static_cast<std::size_t>(n))) {
      K230_LOG_ERROR(kTag) << tag << " stream error: " << demuxer.error();
      break;
    }
  }
  --live_readers_;
}

bool ScrcpySession::running() const {
  return !stop_ && live_readers_ == (config_.audio ? 2 : 1) && server_.running();
}

void ScrcpySession::stop() {
  stop_ = true;
  video_socket_.shutdown();
  audio_socket_.shutdown();
  if (video_thread_.joinable()) video_thread_.join();
  if (audio_thread_.joinable()) audio_thread_.join();
  video_socket_.close();
  audio_socket_.close();
  server_.terminate();
  if (!serial_.empty()) {
    adb_.forward_remove(serial_, config_.video_port);
    if (config_.audio) adb_.forward_remove(serial_, config_.audio_port);
    serial_.clear();
  }
}

std::optional<VideoSessionInfo> ScrcpySession::video_session() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return video_session_;
}

ScrcpyDemuxer::Stats ScrcpySession::video_stats() const {
  return video_demuxer_ ? video_demuxer_->stats() : ScrcpyDemuxer::Stats{};
}

ScrcpyDemuxer::Stats ScrcpySession::audio_stats() const {
  return audio_demuxer_ ? audio_demuxer_->stats() : ScrcpyDemuxer::Stats{};
}

}  // namespace k230::bridge
