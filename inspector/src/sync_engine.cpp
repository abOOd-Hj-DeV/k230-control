#include "k230/inspector/sync_engine.hpp"

#include <utility>

namespace k230::inspector {

SyncEngine::SyncEngine(SyncConfig config)
    : config_(config), ring_(config.audio_before_us + config.audio_after_us + 2'000'000) {}

void SyncEngine::push_video(VideoFrame frame, std::int64_t now_wall_us) {
  ++stats_.frames_in;
  pending_.push_back(Pending{std::move(frame), now_wall_us});
}

void SyncEngine::push_audio(PcmChunk chunk) {
  ++stats_.audio_chunks_in;
  ring_.push(std::move(chunk));
}

SyncedSample SyncEngine::release(Pending&& p) {
  SyncedSample s;
  s.frame = std::move(p.frame);
  if (config_.audio_enabled) {
    s.audio_start_us = s.frame.pts_us - config_.audio_before_us;
    s.audio_end_us = s.frame.pts_us + config_.audio_after_us;
    s.sample_rate = ring_.sample_rate();
    s.channels = ring_.channels();
    s.audio = ring_.extract(s.audio_start_us, s.audio_end_us, &s.audio_filled_us);
    s.audio_complete = s.audio_filled_us >= s.audio_end_us - s.audio_start_us;
    stats_.last_av_lead_us = ring_.newest_end_pts_us() - s.frame.pts_us;
  } else {
    s.audio_complete = true;
  }
  ++stats_.emitted;
  if (!s.audio_complete) ++stats_.emitted_incomplete;
  return s;
}

void SyncEngine::poll(std::int64_t now_wall_us, std::vector<SyncedSample>& out) {
  while (!pending_.empty()) {
    Pending& p = pending_.front();

    if (!config_.audio_enabled) {
      out.push_back(release(std::move(p)));
      pending_.pop_front();
      continue;
    }

    const std::int64_t need_until = p.frame.pts_us + config_.audio_after_us;
    if (!ring_.empty() && ring_.newest_end_pts_us() >= need_until) {
      out.push_back(release(std::move(p)));
      pending_.pop_front();
      continue;
    }

    const bool timed_out = now_wall_us - p.arrival_wall_us >= config_.max_wait_us;
    const bool too_many = pending_.size() > config_.max_pending_frames;
    if (timed_out || too_many) {
      if (too_many && !timed_out) ++stats_.emitted_backpressure;
      out.push_back(release(std::move(p)));
      pending_.pop_front();
      continue;
    }
    break;  // oldest frame still waiting for audio; younger ones must wait too
  }
}

void SyncEngine::flush(std::vector<SyncedSample>& out) {
  while (!pending_.empty()) {
    Pending& p = pending_.front();
    out.push_back(release(std::move(p)));
    pending_.pop_front();
  }
}

}  // namespace k230::inspector
