#pragma once

#include <cstdint>
#include <unordered_map>

#include "k230/inspector/analyzer.hpp"
#include "k230/verdict.hpp"

namespace k230::inspector {

// Turns per-frame scores into actions, with hysteresis so that a single noisy
// frame never triggers an intervention on the phone.
//
//   score >= block_threshold for `confirm_frames` observations in one region -> Block
//   score >= warn_threshold  for `confirm_frames` observations in one region -> Warn
//   otherwise                                                           -> Log
//
// After a Block/Warn is issued, the same category stays silent for
// `cooldown_us` (PTS domain) so the companion app is not flooded.
struct PolicyConfig {
  float warn_threshold = 0.60f;
  float block_threshold = 0.85f;
  std::uint32_t confirm_frames = 3;
  std::int64_t cooldown_us = 5'000'000;
  Action escalated_action = Action::Block;  // what "block" means for this deployment (Block or Delete)
};

class Policy {
 public:
  explicit Policy(PolicyConfig config) : config_(config) {}

  Verdict evaluate(std::int64_t pts_us, const Scores& scores);
  const PolicyConfig& config() const { return config_; }

 private:
  struct Track {
    std::uint32_t warn_streak = 0;
    std::uint32_t block_streak = 0;
  };

  struct TrackGroup {
    std::unordered_map<std::uint32_t, Track> regions;
    std::uint64_t layout = 0;
    std::int64_t silenced_until_us = -1;
  };

  Action step(TrackGroup& group, float score, std::int64_t pts_us, std::uint32_t region,
              std::uint64_t layout);

  PolicyConfig config_;
  TrackGroup nudity_;
  TrackGroup violence_;
  TrackGroup profanity_;
  std::uint32_t sequence_ = 0;
  std::int64_t last_pts_us_ = -1;
};

}  // namespace k230::inspector
