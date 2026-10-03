#include "k230/inspector/protection_session.hpp"

#include <algorithm>

namespace k230::inspector {
using companion::Json;
ProtectionSession::ProtectionSession(std::shared_ptr<ipc::ControlSink> downstream, bool verified_pts_clock)
  : downstream_(std::move(downstream)), clock_verified_(verified_pts_clock), stream_id_(companion::uuid()) {}
ProtectionSession::Stats ProtectionSession::stats() const { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
std::int64_t ProtectionSession::phone_now() const {
  auto base = companion::decimal(state_.at("phone_time_us"));
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-state_received_).count();
  if (base > INT64_MAX - elapsed) throw std::runtime_error("bounds");
  return base + elapsed;
}
void ProtectionSession::reconcile_protection() {
  emitted_stage_ = native_stage_; masks_.clear(); mask_pts_ = -1;
  for (const auto& entry : claims_) {
    const auto& claim = entry.second;
    emitted_stage_ = std::max(emitted_stage_,claim.stage);
    masks_.insert(masks_.end(),claim.masks.begin(),claim.masks.end());
    if (mask_pts_ < 0 || claim.pts < mask_pts_) mask_pts_ = claim.pts;
  }
}
void ProtectionSession::disconnected() {
  std::lock_guard<std::mutex> lock(mutex_);
  bound_ = false; hello_ = nullptr; state_ = nullptr; policy_.reset_evidence();
  // Never replay outstanding commands across a connection; execution may be unknown.
  stats_.unknown += pending_.size(); pending_.clear();
}
void ProtectionSession::receive(const Json& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  try {
    companion::validate(message); auto type = message.at("type").get<std::string>();
    if (type == "hello") {
      hello_ = message; bound_ = false; state_ = nullptr; receive_seq_ = 0; send_seq_ = 0;
      policy_.reset_evidence(); stats_.unknown += pending_.size(); pending_.clear();
      if (!clock_verified_) return;
      Json bind{{"v",2},{"type","bind"},{"session_id",hello_.at("session_id")},{"seq",std::to_string(++send_seq_)},
        {"stream_id",stream_id_},{"pts_clock","android_system_nano_time_us"},
        {"capture",{{"source","scrcpy-4.0-display"},{"display_id",0},{"mirror",false},{"custom_crop",false},{"custom_rotation",false}}}};
      bind_seq_ = send_seq_; if (!downstream_->push(std::move(bind))) ++stats_.rejected; return;
    }
    if (hello_.is_null() || message.at("session_id") != hello_.at("session_id") || message.at("stream_id") != stream_id_) { ++stats_.rejected; return; }
    auto seq = companion::decimal(message.at("seq"),true);
    if (seq <= receive_seq_) { ++stats_.rejected; return; } receive_seq_ = seq;
    if (type == "bound") {
      bound_ = message.at("status") == "accepted" && companion::decimal(message.at("request_seq")) == bind_seq_; return;
    }
    if (!bound_) { ++stats_.rejected; return; }
    if (type == "state") {
      // Stale timestamps or policy rollback cannot refresh an execution gate.
      if (!state_.is_null() && companion::decimal(message.at("phone_time_us")) < companion::decimal(state_.at("phone_time_us"))) { ++stats_.rejected; return; }
      if (!message.at("policy").is_null()) {
        auto p = companion::profile_from_json(message.at("policy"));
        if (!policy_.set_profile(p.age,p.revision)) { ++stats_.rejected; return; }
      } else policy_.reset_evidence();
      state_ = message; state_received_ = std::chrono::steady_clock::now();
      const auto& protection = state_.at("protection");
      if (protection.at("stage").get<int>() > 0) {
        if (event_id_.empty()) event_id_ = protection.at("event_id");
        native_stage_ = std::max(native_stage_,protection.at("stage").get<int>());
        reconcile_protection();
      }
      return;
    }
    if (type == "released") {
      if (message.at("event_id") != event_id_ ||
          (revision_ > 0 && companion::decimal(message.at("action_revision")) != revision_) ||
          (!event_screen_token_.empty() && message.at("previous_screen_token") != event_screen_token_)) {
        ++stats_.rejected; return;
      }
      event_screen_token_.clear(); stats_.unknown += pending_.size();
      event_id_.clear(); revision_ = 0; emitted_stage_ = 0; masks_.clear(); mask_pts_ = -1;
      claims_.clear(); native_stage_ = 0;
      pending_.clear(); policy_.reset_evidence(); return;
    }
    if (type != "ack") { ++stats_.rejected; return; }
    auto revision = companion::decimal(message.at("action_revision")); auto it = pending_.find(revision);
    if (it == pending_.end()) { ++stats_.rejected; return; }
    const auto& command = it->second.command;
    for (auto k : {"event_id","action_revision","screen_token","requested_stage"})
      if (message.at(k) != command.at(k)) { ++stats_.rejected; return; }
    if (message.at("request_seq") != command.at("seq")) { ++stats_.rejected; return; }
    if (message.at("status") == "pending") return;
    auto actual = message.at("executed_stage").get<int>();
    if ((message.at("status") == "executed" || message.at("status") == "duplicate") &&
        actual == command.at("requested_stage").get<int>() && message.at("error").is_null()) {
      ++stats_.executed;
      policy_.executed(event_id_,command.at("package"),actual,companion::decimal(message.at("executed_at_us")));
    } else {
      ++stats_.failed;
      if (message.at("status") == "rejected" || message.at("status") == "failed") {
        if (actual == 0) claims_.erase(revision);
        else {
          auto claim = claims_.find(revision);
          if (claim != claims_.end()) {
            claim->second.stage = actual;
            if (!message.at("display_rects").empty()) {
              claim->second.masks.clear();
              for (const auto& rect : message.at("display_rects")) claim->second.masks.push_back(companion::rect_from_json(rect));
            }
          }
        }
        reconcile_protection(); policy_.reset_evidence();
      }
    }
    pending_.erase(it);
  } catch (const std::exception&) { ++stats_.rejected; }
}
void ProtectionSession::expire_pending() {
  auto now = std::chrono::steady_clock::now();
  for (auto it = pending_.begin(); it != pending_.end();) {
    auto timeout = it->second.command.at("requested_stage") == 3 ? 1500 : 500;
    if (now-it->second.submitted > std::chrono::milliseconds(timeout)) { ++stats_.unknown; it = pending_.erase(it); }
    else ++it;
  }
}
Json ProtectionSession::make_decision(const AnalysisBatch& b, const AgeDecision& d, const std::vector<RegionProof>& proofs) {
  const auto& screen = state_.at("screen");
  Json regions = Json::array();
  for (const auto& p : proofs) regions.push_back({{"track_id",std::to_string(p.track_id)},{"kind",p.region.kind},
    {"crop_frame_px",companion::rect_json(p.region.crop)},{"scores",companion::scores_json(p.region.scores)},
    {"evidence",{{"route",p.region.scores.hentai_dominant()?"hentai_dominant":"explicit"},
      {"observations",p.observations},{"analysis_continuity_id",p.continuity_id},{"analysis_complete",true}}}});
  return {{"v",2},{"type","decision"},{"session_id",hello_.at("session_id")},{"seq",std::to_string(send_seq_+1)},
    {"stream_id",stream_id_},{"event_id",event_id_},{"action_revision",std::to_string(revision_+1)},
    {"pts_us",std::to_string(b.pts_us)},{"expires_at_us",std::to_string(b.pts_us+companion::kTtlUs)},
    {"screen_token",screen.at("screen_token")},{"content_epoch",screen.at("content_epoch")},{"package",screen.at("package")},
    {"window_id",screen.at("window_id")},{"policy",state_.at("policy")},
    {"frame",{{"width",b.width},{"height",b.height},{"display_rotation_deg",screen.at("rotation_deg")}}},
    {"transform",{{"rotation_cw_deg",0},{"viewport_display_px",companion::rect_json({0,0,screen.at("width"),screen.at("height")})}}},
    {"requested_stage",d.stage},{"requested_action",d.stage==1?"cover_region":d.stage==2?"calm_shield":"home"},
    {"reason",proofs.front().repetition?"repetition":"threshold"},{"regions",regions}};
}
void ProtectionSession::analyze(AnalysisBatch batch) {
  std::lock_guard<std::mutex> lock(mutex_); expire_pending();
  try {
    if (!bound_ || state_.is_null() || state_.at("policy").is_null() || state_.at("screen").is_null()) { policy_.reset_evidence(); return; }
    const auto& screen = state_.at("screen"); auto now = phone_now();
    if (screen.at("status") != "verified" || state_.at("health").at("accessibility") != true ||
        now-companion::decimal(screen.at("sampled_at_us")) > 500000 ||
        batch.pts_us < companion::decimal(screen.at("valid_from_us")) || batch.pts_us < 0 ||
        batch.pts_us > INT64_MAX-companion::kTtlUs || (batch.pts_us > now && batch.pts_us-now > 50000) ||
        (now > batch.pts_us && now-batch.pts_us > companion::kTtlUs)) { policy_.reset_evidence(); return; }
    batch.package = screen.at("package");
    batch.identity = stream_id_ + screen.at("screen_token").get<std::string>() + screen.at("content_epoch").get<std::string>() +
      state_.at("policy").dump() + std::to_string(batch.width) + ":" + std::to_string(batch.height);
    companion::Rect viewport{0,0,screen.at("width"),screen.at("height")};
    for (auto& r : batch.regions) {
      auto mapped = companion::map_rect(r.crop,batch.width,batch.height,0,viewport);
      auto overlap = [&](companion::Rect m) { return mapped.x < m.x+m.width && m.x < mapped.x+mapped.width &&
        mapped.y < m.y+m.height && m.y < mapped.y+mapped.height; };
      if (batch.pts_us >= mask_pts_ && mask_pts_ >= 0)
        r.masked |= emitted_stage_ >= 2 || std::any_of(masks_.begin(),masks_.end(),overlap);
      const auto& protection = state_.at("protection");
      if (!protection.at("applied_at_us").is_null() && batch.pts_us >= companion::decimal(protection.at("applied_at_us"))) {
        r.masked |= protection.at("stage").get<int>() >= 2;
        for (const auto& mask : protection.at("covered_rects")) r.masked |= overlap(companion::rect_from_json(mask));
      }
    }
    auto decision = policy_.evaluate(batch);
    if (decision.stage == 0 || decision.stage < emitted_stage_ || !pending_.empty()) return;
    if (decision.stage == emitted_stage_ && decision.stage > 1) return;
    if (event_id_.empty()) event_id_ = companion::uuid();
    std::vector<RegionProof> candidates;
    for (const auto& p : decision.regions) {
      if (decision.stage == 1) {
        auto mapped = companion::map_rect(p.region.crop,batch.width,batch.height,0,viewport);
        if (std::find(masks_.begin(),masks_.end(),mapped) != masks_.end()) continue;
        if (masks_.size()+candidates.size() >= 8) { ++stats_.rejected; return; }
      }
      candidates.push_back(p);
    }
    if (!candidates.empty()) {
      auto command = make_decision(batch,decision,candidates);
      if (command.dump().size() >= companion::kMaxLine) { ++stats_.rejected; return; }
      auto rects = companion::validate_decision(command,state_,now,clock_verified_);
      auto caps = hello_.at("capabilities");
      if (std::find(caps.begin(),caps.end(),command.at("requested_action")) == caps.end()) { ++stats_.rejected; return; }
      companion::line(command);
      if (!downstream_->push(Json(command))) { ++stats_.rejected; return; }
      ++send_seq_; ++revision_; ++stats_.submitted;
      event_screen_token_ = command.at("screen_token");
      pending_[revision_] = {command,std::chrono::steady_clock::now()};
      claims_[revision_] = {decision.stage,std::move(rects),batch.pts_us};
      reconcile_protection();
    }
  } catch (const std::exception&) { ++stats_.rejected; policy_.reset_evidence(); }
}
}  // namespace k230::inspector
