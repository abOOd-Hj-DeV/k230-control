#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "k230/inspector/analyzer.hpp"
#include "k230/inspector/decoder.hpp"
#include "k230/inspector/frame_dump.hpp"
#include "k230/inspector/policy.hpp"
#include "k230/inspector/sync_engine.hpp"
#include "k230/inspector/fast_vision.hpp"
#include "k230/ipc/channel.hpp"

namespace k230::inspector {

struct PipelineConfig {
  SyncConfig sync;
  PolicyConfig policy;
  bool allow_analyzer_fallback = true;
  std::string dump_dir;         // PNGs up to 10 fps and continuous PTS-aligned WAV segments
  std::int64_t idle_timeout_ms = 0;  // stop when no packet arrives for this long (0 = never)
};

// Big-core main loop:
//
//   PacketSource --> VideoDecoder --> SyncEngine --> Analyzer --> Policy --> VerdictSink
//                \-> AudioDecoder ------^
//
// Single-threaded by design: the K230 VDEC and KPU are both asynchronous
// hardware units, so the CPU work here is small and ordering is what matters.
class InspectorPipeline {
 public:
  struct Stats {
    std::uint64_t packets = 0;
    std::uint64_t video_packets = 0;
    std::uint64_t audio_packets = 0;
    std::uint64_t frames_decoded = 0;
    std::uint64_t video_gaps = 0;     // seq discontinuities (packets lost upstream)
    std::uint64_t video_skipped = 0;  // packets skipped while waiting for a key frame
    std::uint64_t samples_analyzed = 0;
    std::uint64_t verdicts_escalated = 0;  // Warn or above
  };

  using SampleObserver = std::function<void(const SyncedSample&, const Scores&, const Verdict&)>;

  InspectorPipeline(PipelineConfig config, std::shared_ptr<ipc::PacketSource> source,
                    std::shared_ptr<ipc::VerdictSink> verdicts, std::unique_ptr<VideoDecoder> video_decoder,
                    std::unique_ptr<Analyzer> analyzer);

  void set_observer(SampleObserver observer) { observer_ = std::move(observer); }
  void set_vision(std::unique_ptr<FastVision> vision) { vision_ = std::move(vision); }
  void set_capture_pts_observer(std::function<void(std::int64_t)> observer) { capture_pts_observer_ = std::move(observer); }

  // Blocks until the source is closed and drained, or request_stop() is called.
  bool run();
  void request_stop() { stop_ = true; }

  const Stats& stats() const { return stats_; }
  const FastVision::Stats* vision_stats() const { return vision_ ? &vision_->stats() : nullptr; }
  const SyncEngine& sync() const { return sync_; }

 private:
  void run_loop();
  void handle(const MediaPacket& packet);
  void drain(std::int64_t now_wall_us);
  void emit(SyncedSample&& sample);
  static std::int64_t now_us();

  PipelineConfig config_;
  std::shared_ptr<ipc::PacketSource> source_;
  std::shared_ptr<ipc::VerdictSink> verdicts_;
  std::unique_ptr<VideoDecoder> video_decoder_;
  std::unique_ptr<AudioDecoder> audio_decoder_;
  std::unique_ptr<Analyzer> analyzer_;
  SyncEngine sync_;
  Policy policy_;
  std::unique_ptr<ContinuousCapture> capture_;
  std::unique_ptr<FastVision> vision_;
  SampleObserver observer_;
  std::function<void(std::int64_t)> capture_pts_observer_;
  std::atomic<bool> stop_{false};
  bool video_open_ = false;
  bool wait_key_frame_ = false;
  std::uint32_t next_video_seq_ = 0;
  bool audio_failed_ = false;
  Stats stats_;
};

}  // namespace k230::inspector
