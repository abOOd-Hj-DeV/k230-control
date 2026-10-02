#include "k230/inspector/pipeline.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>

#include "k230/inspector/frame_dump.hpp"
#include "k230/log.hpp"

namespace k230::inspector {

namespace {
constexpr const char* kTag = "inspector";
}

InspectorPipeline::InspectorPipeline(PipelineConfig config, std::shared_ptr<ipc::PacketSource> source,
                                     std::shared_ptr<ipc::VerdictSink> verdicts,
                                     std::unique_ptr<VideoDecoder> video_decoder, std::unique_ptr<Analyzer> analyzer)
    : config_(std::move(config)),
      source_(std::move(source)),
      verdicts_(std::move(verdicts)),
      video_decoder_(std::move(video_decoder)),
      analyzer_(std::move(analyzer)),
      sync_(config_.sync),
      policy_(config_.policy) {
  if (!config_.dump_dir.empty()) {
    capture_ = std::make_unique<ContinuousCapture>(config_.dump_dir, config_.sync.audio_enabled);
  }
}

std::int64_t InspectorPipeline::now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool InspectorPipeline::run() {
  try {
    if (!analyzer_->open()) {
      if (!config_.allow_analyzer_fallback) throw std::runtime_error("requested analyzer failed to open");
      K230_LOG_WARN(kTag) << "analyzer '" << analyzer_->name() << "' failed to open, falling back to heuristic";
      analyzer_ = std::make_unique<HeuristicAnalyzer>();
      analyzer_->open();
    }
    run_loop();
    return true;
  } catch (const std::exception& e) {
    K230_LOG_ERROR(kTag) << "pipeline failed: " << e.what();
    verdicts_->close();
    return false;
  }
}

void InspectorPipeline::run_loop() {
  K230_LOG_INFO(kTag) << "decoder=" << video_decoder_->name() << " analyzer=" << analyzer_->name();
  if (capture_) {
    std::error_code ec;
    std::filesystem::create_directories(config_.dump_dir, ec);
    if (ec) {
      K230_LOG_ERROR(kTag) << "cannot create dump dir " << config_.dump_dir << ": " << ec.message();
    } else {
      K230_LOG_INFO(kTag) << "dumping PNGs up to 10 fps and continuous audio to "
                          << std::filesystem::absolute(config_.dump_dir).string();
    }
  }

  std::int64_t last_packet_wall = now_us();
  while (!stop_) {
    auto packet = source_->pop(std::chrono::milliseconds(50));
    const std::int64_t now = now_us();
    if (packet) {
      last_packet_wall = now;
      handle(*packet);
    } else if (source_->closed()) {
      break;
    } else if (config_.idle_timeout_ms > 0 && now - last_packet_wall > config_.idle_timeout_ms * 1000) {
      K230_LOG_WARN(kTag) << "no packets for " << config_.idle_timeout_ms << " ms, stopping";
      break;
    }
    drain(now);
  }

  std::vector<VideoFrame> frames;
  if (video_open_) video_decoder_->flush(frames);
  for (auto& f : frames) {
    if (capture_) capture_->push_frame(f);
    sync_.push_video(std::move(f), now_us());
  }
  std::vector<SyncedSample> samples;
  sync_.flush(samples);
  for (auto& s : samples) emit(std::move(s));
  if (capture_ && !capture_->finish()) {
    K230_LOG_WARN(kTag) << "capture incomplete: images=" << capture_->images_written()
                         << " audio=" << capture_->audio_filled_us() / 1000 << "/"
                         << capture_->audio_duration_us() / 1000 << "ms";
  }
  verdicts_->close();
  K230_LOG_INFO(kTag) << "done: packets=" << stats_.packets << " frames=" << stats_.frames_decoded
                      << " analyzed=" << stats_.samples_analyzed << " escalated=" << stats_.verdicts_escalated
                      << " video_gaps=" << stats_.video_gaps << " skipped=" << stats_.video_skipped;
}

void InspectorPipeline::handle(const MediaPacket& packet) {
  ++stats_.packets;
  if (packet.stream == StreamType::Video) {
    ++stats_.video_packets;
    if (!video_open_) {
      if (!video_decoder_->open(packet.codec)) {
        K230_LOG_ERROR(kTag) << "cannot open video decoder for " << to_string(packet.codec);
        stop_ = true;
        return;
      }
      video_open_ = true;
    }
    if (stats_.video_packets > 1 && packet.seq != 0 && packet.seq != next_video_seq_) {
      ++stats_.video_gaps;
      wait_key_frame_ = true;
    }
    next_video_seq_ = packet.seq + 1;
    if (wait_key_frame_) {
      if (!packet.is_config && !packet.is_key_frame) {
        ++stats_.video_skipped;
        return;
      }
      if (packet.is_key_frame) wait_key_frame_ = false;
    }
    std::vector<VideoFrame> frames;
    if (!video_decoder_->decode(packet, frames)) {
      K230_LOG_WARN(kTag) << "video decoder failure; waiting for key frame";
      wait_key_frame_ = true;
      return;
    }
    const std::int64_t now = now_us();
    for (auto& f : frames) {
      ++stats_.frames_decoded;
      if (capture_) capture_->push_frame(f);
      sync_.push_video(std::move(f), now);
    }
    return;
  }

  ++stats_.audio_packets;
  if (audio_failed_) return;
  if (!audio_decoder_) {
    audio_decoder_ = make_default_audio_decoder(packet.codec);
    if (!audio_decoder_ || !audio_decoder_->open(packet.codec)) {
      K230_LOG_WARN(kTag) << "audio disabled: no decoder for " << to_string(packet.codec);
      audio_failed_ = true;
      return;
    }
  }
  std::vector<PcmChunk> chunks;
  if (!audio_decoder_->decode(packet, chunks)) {
    K230_LOG_WARN(kTag) << "audio decoder failure, audio disabled";
    audio_failed_ = true;
    return;
  }
  for (auto& c : chunks) {
    if (capture_) capture_->push_audio(c);
    sync_.push_audio(std::move(c));
  }
}

void InspectorPipeline::drain(std::int64_t now_wall_us) {
  std::vector<SyncedSample> samples;
  sync_.poll(now_wall_us, samples);
  for (auto& s : samples) emit(std::move(s));
}

void InspectorPipeline::emit(SyncedSample&& sample) {
  const Scores scores = analyzer_->analyze(sample);
  Verdict verdict = policy_.evaluate(sample.frame.pts_us, scores);
  ++stats_.samples_analyzed;
  if (verdict.action >= Action::Warn) ++stats_.verdicts_escalated;

  if (observer_) observer_(sample, scores, verdict);
  verdicts_->push(std::move(verdict));
}

}  // namespace k230::inspector
