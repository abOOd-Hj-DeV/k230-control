#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "k230/inspector/frame.hpp"
#include "k230/inspector/pcm_ring.hpp"

namespace k230::inspector {

// Audio/video alignment on the phone's clock.
//
// scrcpy stamps both video access units (MediaCodec presentationTimeUs) and
// audio packets (AudioRecord timestamp) with the phone's CLOCK_MONOTONIC in
// microseconds, so the two streams share one time base and can be aligned by
// PTS alone - no wall-clock guessing on the receiver.
//
// For every decoded frame we wait until the audio ring holds samples up to
// frame.pts + audio_after_us, then emit the frame together with the PCM in
// [frame.pts - audio_before_us, frame.pts + audio_after_us). If the audio
// stalls, the frame is released after `max_wait_us` of wall time with the
// missing part zero-filled, so a broken microphone can never freeze the video
// analysis.
struct SyncConfig {
  bool audio_enabled = true;
  std::int64_t audio_before_us = 2'500'000;
  std::int64_t audio_after_us = 500'000;
  std::int64_t max_wait_us = 1'000'000;
  std::size_t max_pending_frames = 8;
};

struct SyncedSample {
  VideoFrame frame;
  std::vector<std::int16_t> audio;  // interleaved s16, may be empty when audio is disabled
  std::int64_t audio_start_us = 0;
  std::int64_t audio_end_us = 0;
  std::int64_t audio_filled_us = 0;  // how much of the window had real samples
  bool audio_complete = false;
  std::uint32_t sample_rate = 48000;
  std::uint16_t channels = 2;
};

class SyncEngine {
 public:
  struct Stats {
    std::uint64_t frames_in = 0;
    std::uint64_t audio_chunks_in = 0;
    std::uint64_t emitted = 0;
    std::uint64_t emitted_incomplete = 0;
    std::uint64_t emitted_backpressure = 0;
    std::int64_t last_av_lead_us = 0;  // audio_end - frame.pts at emission time
  };

  explicit SyncEngine(SyncConfig config);

  // `now_wall_us` is the receiver's monotonic clock, only used for timeouts.
  void push_video(VideoFrame frame, std::int64_t now_wall_us);
  void push_audio(PcmChunk chunk);

  // Moves every frame that can be released into `out`.
  void poll(std::int64_t now_wall_us, std::vector<SyncedSample>& out);
  // Release everything, regardless of audio availability.
  void flush(std::vector<SyncedSample>& out);

  const Stats& stats() const { return stats_; }
  std::size_t pending() const { return pending_.size(); }
  const PcmRing& ring() const { return ring_; }

 private:
  struct Pending {
    VideoFrame frame;
    std::int64_t arrival_wall_us;
  };

  SyncedSample release(Pending&& p, bool force);

  SyncConfig config_;
  PcmRing ring_;
  std::deque<Pending> pending_;
  Stats stats_;
};

}  // namespace k230::inspector
