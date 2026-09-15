#include "k230/inspector/pipeline.hpp"

#include <chrono>

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
      policy_(config_.policy) {}

std::int64_t InspectorPipeline::now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void InspectorPipeline::run() {
  if (!analyzer_->open()) {
    K230_LOG_WARN(kTag) << "analyzer '" << analyzer_->name() << "' failed to open, falling back to heuristic";
    analyzer_ = std::make_unique<HeuristicAnalyzer>();
    analyzer_->open();
  }
  K230_LOG_INFO(kTag) << "decoder=" << video_decoder_->name() << " analyzer=" << analyzer_->name();

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
  for (auto& f : frames) sync_.push_video(std::move(f), now_us());
  std::vector<SyncedSample> samples;
  sync_.flush(samples);
  for (auto& s : samples) emit(std::move(s));
  verdicts_->close();
  K230_LOG_INFO(kTag) << "done: packets=" << stats_.packets << " frames=" << stats_.frames_decoded
                      << " analyzed=" << stats_.samples_analyzed << " escalated=" << stats_.verdicts_escalated;
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
    std::vector<VideoFrame> frames;
    if (!video_decoder_->decode(packet, frames)) {
      K230_LOG_ERROR(kTag) << "video decoder failure";
      stop_ = true;
      return;
    }
    const std::int64_t now = now_us();
    for (auto& f : frames) {
      ++stats_.frames_decoded;
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
  for (auto& c : chunks) sync_.push_audio(std::move(c));
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

  if (!config_.dump_dir.empty() && config_.dump_every > 0 && stats_.samples_analyzed % config_.dump_every == 0) {
    const std::string base = config_.dump_dir + "/sample_" + std::to_string(sample.frame.pts_us);
    write_ppm(sample.frame, base + ".ppm");
    if (!sample.audio.empty()) write_wav(sample.audio, sample.sample_rate, sample.channels, base + ".wav");
  }

  if (observer_) observer_(sample, scores, verdict);
  verdicts_->push(std::move(verdict));
}

}  // namespace k230::inspector
