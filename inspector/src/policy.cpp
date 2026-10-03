#include "k230/inspector/policy.hpp"

namespace k230::inspector {

Action Policy::step(TrackGroup& group, float score, std::int64_t pts_us, std::uint32_t region,
                    std::uint64_t layout) {
  if (group.layout != layout) {
    group.regions.clear();
    group.layout = layout;
    group.silenced_until_us = -1;
  }
  auto& t = group.regions[region];
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

  if (group.silenced_until_us >= 0 && pts_us < group.silenced_until_us) return Action::Log;

  if (t.block_streak >= config_.confirm_frames) {
    group.silenced_until_us = pts_us + config_.cooldown_us;
    group.regions.clear();
    return config_.escalated_action;
  }
  if (t.warn_streak >= config_.confirm_frames) {
    group.silenced_until_us = pts_us + config_.cooldown_us;
    group.regions.clear();
    return Action::Warn;
  }
  return Action::Log;
}

Verdict Policy::evaluate(std::int64_t pts_us, const Scores& scores) {
  if (pts_us < 0 || pts_us <= last_pts_us_) {
    Verdict ignored;
    ignored.pts_us = pts_us;
    ignored.category = Category::Unknown;
    ignored.sequence = ++sequence_;
    return ignored;
  }
  last_pts_us_ = pts_us;
  struct Candidate {
    Category category;
    float score;
    Action action;
  };
  const Candidate candidates[] = {
      {Category::Nudity, scores.nudity,
       step(nudity_, scores.nudity, pts_us, scores.analysis_region, scores.analysis_layout)},
      {Category::Violence, scores.violence,
       step(violence_, scores.violence, pts_us, scores.analysis_region, scores.analysis_layout)},
      {Category::Profanity, scores.profanity,
       step(profanity_, scores.profanity, pts_us, scores.analysis_region, scores.analysis_layout)},
  };

  Verdict v;
  v.pts_us = pts_us;
  v.sequence = ++sequence_;
  v.action = Action::Log;
  v.category = scores.analysis_complete ? Category::Safe : Category::Unknown;
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
