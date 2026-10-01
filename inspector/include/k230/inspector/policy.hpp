#pragma once

#include <cstdint>

#include "k230/inspector/analyzer.hpp"
#include "k230/verdict.hpp"

namespace k230::inspector {

// Turns per-frame scores into actions, with hysteresis so that a single noisy
// frame never triggers an intervention on the phone.
//
//   score >= block_threshold for `confirm_frames` consecutive frames -> Block
//   score >= warn_threshold  for `confirm_frames` consecutive frames -> Warn
//   otherwise                                                        -> Log
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
    std::int64_t silenced_until_us = -1;
  };

  Action step(Track& t, float score, std::int64_t pts_us);

  PolicyConfig config_;
  Track nudity_;
  Track violence_;
  Track profanity_;
  std::uint32_t sequence_ = 0;
};

}  // namespace k230::inspector
