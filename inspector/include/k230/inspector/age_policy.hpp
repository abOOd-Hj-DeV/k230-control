#pragma once

#include <deque>
#include <map>
#include <optional>
#include "k230/companion_v2.hpp"

namespace k230::inspector {
struct RegionObservation {
  companion::Rect crop;
  std::string kind = "Image";
  companion::Probabilities scores;
  bool complete = true;
  bool masked = false;
};
struct AnalysisBatch {
  std::int64_t pts_us = -1;
  int width = 0, height = 0;
  std::string identity;
  std::string package;
  bool complete = false, discontinuity = false;
  std::vector<RegionObservation> regions;
};
struct RegionProof {
  std::uint64_t track_id = 0;
  RegionObservation region;
  std::string continuity_id;
  std::deque<companion::Json> observations;
  int stage = 0;
  bool repetition = false;
};
struct AgeDecision {
  int stage = 0;
  bool safe = false, deferred = false;
  std::vector<RegionProof> regions;
};

class AgePolicy {
 public:
  // Invalid ages/revisions leave the last valid profile unchanged.
  bool set_profile(int age, std::int64_t revision);
  const std::optional<companion::AgeProfile>& profile() const { return profile_; }
  AgeDecision evaluate(const AnalysisBatch& batch, const std::string& episode_id = {},
                       std::int64_t phone_now_us = -1);
  void executed(const std::string& event_id, const std::string& package, int stage, std::int64_t phone_time_us);
  void reset_evidence();
  void reset_repetition() { events_.clear(); }
 private:
  struct Chain {
    std::deque<companion::Json> observations;
    std::int64_t started_pts = -1;
    void add(bool qualifies, std::int64_t pts, companion::Probabilities scores);
    bool ready(std::size_t count, std::int64_t span) const;
  };
  struct Track {
    std::uint64_t id = 0;
    RegionObservation region;
    std::string continuity;
    bool hentai = false;
    Chain cover, shield, exit;
  };
  struct Event { std::string id, package; std::int64_t time; };
  std::optional<companion::AgeProfile> profile_;
  std::vector<Track> tracks_;
  std::deque<Event> events_;
  std::uint64_t next_id_ = 1;
  std::int64_t last_pts_ = -1;
  std::string identity_;
};
}  // namespace k230::inspector
