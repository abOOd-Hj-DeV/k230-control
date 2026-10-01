#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "k230/inspector/frame.hpp"

namespace k230::inspector {

// PTS-indexed audio history. Chunks are stored in arrival order (the phone
// produces them monotonically) and trimmed once older than `history_us`.
// Extraction is by time range, so callers never reason about sample indices.
class PcmRing {
 public:
  explicit PcmRing(std::int64_t history_us = 3'000'000) : history_us_(history_us) {}

  void push(PcmChunk chunk);

  bool empty() const { return chunks_.empty(); }
  std::int64_t oldest_pts_us() const;  // -1 when empty
  std::int64_t newest_end_pts_us() const;  // end of the most recent chunk, -1 when empty
  std::uint32_t sample_rate() const { return sample_rate_; }
  std::uint16_t channels() const { return channels_; }

  // Interleaved samples covering [start_us, end_us). Gaps in the source (lost
  // or not-yet-arrived chunks) are zero-filled so the output length always
  // equals the requested duration. `filled_us` reports how much real audio
  // was present.
  std::vector<std::int16_t> extract(std::int64_t start_us, std::int64_t end_us, std::int64_t* filled_us = nullptr) const;

  // Drop everything that ends before `pts_us`.
  void drop_before(std::int64_t pts_us);

  std::size_t chunk_count() const { return chunks_.size(); }

 private:
  std::int64_t history_us_;
  std::deque<PcmChunk> chunks_;
  std::uint32_t sample_rate_ = 48000;
  std::uint16_t channels_ = 2;
};

}  // namespace k230::inspector
