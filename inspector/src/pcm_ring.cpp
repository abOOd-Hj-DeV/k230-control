#include "k230/inspector/pcm_ring.hpp"

#include <algorithm>

namespace k230::inspector {

void PcmRing::push(PcmChunk chunk) {
  if (chunk.samples.empty() || chunk.pts_us < 0) return;
  if (chunks_.empty()) {
    sample_rate_ = chunk.sample_rate;
    channels_ = chunk.channels;
  }
  chunks_.push_back(std::move(chunk));
  const std::int64_t newest = chunks_.back().end_pts_us();
  drop_before(newest - history_us_);
}

std::int64_t PcmRing::oldest_pts_us() const { return chunks_.empty() ? -1 : chunks_.front().pts_us; }

std::int64_t PcmRing::newest_end_pts_us() const { return chunks_.empty() ? -1 : chunks_.back().end_pts_us(); }

void PcmRing::drop_before(std::int64_t pts_us) {
  while (!chunks_.empty() && chunks_.front().end_pts_us() <= pts_us) chunks_.pop_front();
}

std::vector<std::int16_t> PcmRing::extract(std::int64_t start_us, std::int64_t end_us, std::int64_t* filled_us) const {
  std::vector<std::int16_t> out;
  if (filled_us) *filled_us = 0;
  if (end_us <= start_us) return out;

  const auto frames_for = [this](std::int64_t us) {
    return static_cast<std::int64_t>(us * sample_rate_ / 1'000'000);
  };
  const std::int64_t total_frames = frames_for(end_us - start_us);
  out.assign(static_cast<std::size_t>(total_frames) * channels_, 0);

  std::int64_t filled_frames = 0;
  for (const auto& c : chunks_) {
    if (c.end_pts_us() <= start_us) continue;
    if (c.pts_us >= end_us) break;
    const std::int64_t overlap_start = std::max(c.pts_us, start_us);
    const std::int64_t overlap_end = std::min(c.end_pts_us(), end_us);
    const std::int64_t src_frame = frames_for(overlap_start - c.pts_us);
    const std::int64_t dst_frame = frames_for(overlap_start - start_us);
    std::int64_t n = frames_for(overlap_end - overlap_start);
    n = std::min({n, static_cast<std::int64_t>(c.frames()) - src_frame, total_frames - dst_frame});
    if (n <= 0) continue;
    std::copy_n(c.samples.begin() + static_cast<std::ptrdiff_t>(src_frame * channels_),
                static_cast<std::ptrdiff_t>(n * channels_),
                out.begin() + static_cast<std::ptrdiff_t>(dst_frame * channels_));
    filled_frames += n;
  }
  if (filled_us) *filled_us = filled_frames * 1'000'000 / sample_rate_;
  return out;
}

}  // namespace k230::inspector
