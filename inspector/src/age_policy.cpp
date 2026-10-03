#include "k230/inspector/age_policy.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace k230::inspector {
namespace {
double iou(companion::Rect a, companion::Rect b) {
  double intersection = static_cast<double>(std::max(0, std::min(a.x+a.width,b.x+b.width)-std::max(a.x,b.x))) *
    std::max(0, std::min(a.y+a.height,b.y+b.height)-std::max(a.y,b.y));
  return intersection / (static_cast<double>(a.width)*a.height + static_cast<double>(b.width)*b.height - intersection);
}
bool associate(const RegionObservation& a, const RegionObservation& b, int width, int height) {
  if (a.kind != b.kind || a.crop.width <= 0 || a.crop.height <= 0) return false;
  auto r = a.crop, s = b.crop;
  return iou(r,s) >= .70 && std::hypot((r.x+r.width*.5)-(s.x+s.width*.5), (r.y+r.height*.5)-(s.y+s.height*.5)) <=
    .05 * std::hypot(width,height) && std::abs(s.width-r.width) <= .10*r.width && std::abs(s.height-r.height) <= .10*r.height;
}
bool eligible(const RegionObservation& r, int w, int h) {
  return (r.kind == "Image" || r.kind == "BackgroundImage" || r.kind == "Video") && r.crop.x >= 0 && r.crop.y >= 0 &&
    r.crop.width > 0 && r.crop.height > 0 && static_cast<std::int64_t>(r.crop.x)+r.crop.width <= w &&
    static_cast<std::int64_t>(r.crop.y)+r.crop.height <= h;
}
}  // namespace
bool AgePolicy::set_profile(int age, std::int64_t revision) {
  companion::AgeProfile next{age, revision};
  if (!next.valid() || (profile_ && (revision < profile_->revision || (revision == profile_->revision && age != profile_->age)))) return false;
  if (!profile_ || revision != profile_->revision) { profile_ = next; reset_evidence(); events_.clear(); }
  return true;
}
void AgePolicy::reset_evidence() { tracks_.clear(); identity_.clear(); last_pts_ = -1; }
void AgePolicy::Chain::add(bool qualifies, std::int64_t pts, companion::Probabilities scores) {
  if (!qualifies) { observations.clear(); started_pts = -1; return; }
  if (started_pts < 0) started_pts = pts;
  auto o = companion::scores_json(scores, false); o["pts_us"] = std::to_string(pts);
  observations.push_back(std::move(o)); if (observations.size() > 32) observations.pop_front();
}
bool AgePolicy::Chain::ready(std::size_t count, std::int64_t span) const {
  return observations.size() >= count && companion::decimal(observations.back().at("pts_us")) -
    companion::decimal(observations.front().at("pts_us")) >= span;
}
void AgePolicy::executed(const std::string& id, const std::string& package, int stage, std::int64_t time) {
  if (!profile_ || (stage != 1 && stage != 2) || time < 0 || !companion::valid_uuid(id)) return;
  for (const auto& e : events_) if (e.id == id) return;
  events_.push_back({id, package, time});
  while (events_.size() > 1024) events_.pop_front();
}
AgeDecision AgePolicy::evaluate(const AnalysisBatch& batch) {
  AgeDecision out;
  if (!profile_ || batch.pts_us < 0 || batch.pts_us <= last_pts_) return out;
  if (batch.identity != identity_ || batch.discontinuity || (last_pts_ >= 0 && batch.pts_us-last_pts_ > 500000)) {
    tracks_.clear();
  }
  last_pts_ = batch.pts_us; identity_ = batch.identity;
  if (batch.width < 1 || batch.height < 1 || batch.width > 16384 || batch.height > 16384 || batch.regions.size() > 8) {
    tracks_.clear(); return out;
  }
  out.safe = batch.complete && !batch.regions.empty();
  struct Edge { double overlap; std::size_t current, previous; };
  std::vector<Edge> edges;
  std::set<std::size_t> ambiguous_current, ambiguous_previous;
  for (std::size_t i = 0; i < batch.regions.size(); ++i)
    for (std::size_t j = 0; j < tracks_.size(); ++j)
      if (eligible(batch.regions[i],batch.width,batch.height) && associate(tracks_[j].region, batch.regions[i], batch.width, batch.height))
        edges.push_back({iou(tracks_[j].region.crop, batch.regions[i].crop),i,j});
  for (const auto& a : edges) for (const auto& b : edges) {
    if (a.current == b.current && a.previous != b.previous && std::abs(a.overlap-b.overlap) <= .05) ambiguous_current.insert(a.current);
    if (a.previous == b.previous && a.current != b.current && std::abs(a.overlap-b.overlap) <= .05) ambiguous_previous.insert(a.previous);
  }
  std::sort(edges.begin(), edges.end(), [&](const Edge& a, const Edge& b) {
    if (a.overlap != b.overlap) return a.overlap > b.overlap;
    auto r = batch.regions[a.current].crop, s = batch.regions[b.current].crop;
    return std::make_tuple(tracks_[a.previous].id,r.x,r.y,r.width,r.height) < std::make_tuple(tracks_[b.previous].id,s.x,s.y,s.width,s.height);
  });
  std::map<std::size_t,std::size_t> matched; std::set<std::size_t> used;
  for (const auto& e : edges) if (!ambiguous_current.count(e.current) && !ambiguous_previous.count(e.previous) &&
      !matched.count(e.current) && !used.count(e.previous)) { matched[e.current] = e.previous; used.insert(e.previous); }
  std::vector<Track> next;
  int events = 1;
  for (const auto& e : events_) if (e.package == batch.package && e.time <= batch.pts_us && batch.pts_us-e.time < 60000000) ++events;
  for (std::size_t i = 0; i < batch.regions.size(); ++i) {
    const auto& r = batch.regions[i];
    if (!eligible(r,batch.width,batch.height) || !r.complete || !r.scores.valid() || r.masked) { out.safe = false; continue; }
    Track t;
    if (matched.count(i)) t = std::move(tracks_[matched[i]]);
    else { t.id = next_id_++; t.continuity = companion::uuid(); }
    bool hentai = r.scores.hentai_dominant();
    if (t.hentai != hentai) { t.cover = {}; t.shield = {}; t.exit = {}; t.continuity = companion::uuid(); }
    t.hentai = hentai; t.region = r;
    double e = r.scores.explicit_score();
    bool route_valid = !hentai || r.scores.hentai > .60;
    t.cover.add(route_valid && e >= profile_->cover(),batch.pts_us,r.scores);
    t.shield.add(route_valid && e >= profile_->shield(),batch.pts_us,r.scores);
    t.exit.add(!hentai && e >= profile_->exit() && r.scores.porn >= .60,batch.pts_us,r.scores);
    if (e >= profile_->cover()) out.safe = false;
    auto count = hentai ? 5u : 3u; auto span = hentai ? 1000000 : 400000;
    const Chain* proof = nullptr; int stage = 0; bool repeated = false;
    if (t.cover.ready(count,span)) { stage = 1; proof = &t.cover; }
    if (t.shield.ready(count,span)) { stage = 2; proof = &t.shield; }
    if (stage == 1 && events >= profile_->repetition_limit()) { stage = 2; repeated = true; }
    if (t.exit.ready(5,1000000)) { stage = 3; proof = &t.exit; repeated = false; }
    // Only unmasked, currently qualifying exit evidence can delay a proven lower action.
    if (stage < 3 && !t.exit.observations.empty()) {
      const auto first = t.exit.started_pts;
      const auto deadline = first > INT64_MAX-2000000 ? INT64_MAX : first+2000000;
      out.deferred |= batch.pts_us < deadline;
    }
    if (stage) out.regions.push_back({t.id,r,t.continuity,proof->observations,stage,repeated});
    next.push_back(std::move(t));
  }
  tracks_ = std::move(next);
  std::sort(out.regions.begin(),out.regions.end(),[](const auto& a,const auto& b) {
    if (a.stage != b.stage) return a.stage > b.stage;
    if (a.region.scores.explicit_score() != b.region.scores.explicit_score()) return a.region.scores.explicit_score() > b.region.scores.explicit_score();
    return a.track_id < b.track_id;
  });
  if (!out.regions.empty()) {
    out.stage = out.regions.front().stage;
    if (out.stage > 1) out.regions.resize(1);
  }
  if (out.deferred && out.stage < 3) { out.stage = 0; out.regions.clear(); }
  return out;
}
}  // namespace k230::inspector
