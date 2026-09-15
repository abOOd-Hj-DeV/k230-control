#include "k230/inspector/policy.hpp"

namespace k230::inspector {

Action Policy::step(Track& t, float score, std::int64_t pts_us) {
  if (score >= config_.block_threshold) {
    ++t.block_streak;
    ++t.warn_streak;
  } else if (score >= config_.warn_threshold) {
    t.block_streak = 0;
    ++t.warn_streak;
  } else {
    t.block_streak = 0;
    t.warn_streak = 0;
    return Action::Log;
  }

  if (t.silenced_until_us >= 0 && pts_us < t.silenced_until_us) return Action::Log;

  if (t.block_streak >= config_.confirm_frames) {
    t.silenced_until_us = pts_us + config_.cooldown_us;
    t.block_streak = 0;
    t.warn_streak = 0;
    return config_.escalated_action;
  }
  if (t.warn_streak >= config_.confirm_frames) {
    t.silenced_until_us = pts_us + config_.cooldown_us;
    t.warn_streak = 0;
    return Action::Warn;
  }
  return Action::Log;
}

Verdict Policy::evaluate(std::int64_t pts_us, const Scores& scores) {
  struct Candidate {
    Category category;
    float score;
    Action action;
  };
  const Candidate candidates[] = {
      {Category::Nudity, scores.nudity, step(nudity_, scores.nudity, pts_us)},
      {Category::Violence, scores.violence, step(violence_, scores.violence, pts_us)},
      {Category::Profanity, scores.profanity, step(profanity_, scores.profanity, pts_us)},
  };

  Verdict v;
  v.pts_us = pts_us;
  v.sequence = ++sequence_;
  v.action = Action::Log;
  v.category = Category::Safe;
  v.confidence = 1.0f;

  float top_score = 0.0f;
  for (const auto& c : candidates) {
    // The most severe action wins; among equal actions the highest score.
    if (c.action > v.action || (c.action == v.action && c.action != Action::Log && c.score > v.confidence)) {
      v.action = c.action;
      v.category = c.category;
      v.confidence = c.score;
    }
    if (c.score > top_score) top_score = c.score;
  }
  if (v.action == Action::Log) v.confidence = 1.0f - top_score;
  return v;
}

}  // namespace k230::inspector
