#include "k230/inspector/protection_session.hpp"

#include <algorithm>

namespace k230::inspector {
using companion::Json;
ProtectionSession::ProtectionSession(std::shared_ptr<ipc::ControlSink> downstream, bool, Clock clock)
  : downstream_(std::move(downstream)), clock_(std::move(clock)), stream_id_(companion::uuid()) {}
bool ProtectionSession::send_bind(const Json& capture_pts) {
  Json bind{{"v",2},{"type","bind"},{"session_id",hello_.at("session_id")},{"seq",std::to_string(++send_seq_)},
    {"stream_id",stream_id_},{"pts_clock","android_system_nano_time_us"},{"capture_pts_us",capture_pts},
    {"capture",{{"source","scrcpy-4.0-display"},{"display_id",0},{"mirror",false},{"custom_crop",false},{"custom_rotation",false}}}};
  bind_seq_ = send_seq_; return downstream_->push(std::move(bind));
}
void ProtectionSession::observe_capture_pts(std::int64_t pts) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (hello_.is_null() || bound_ || probe_failed_) return;
  auto now = clock_();
  if (pts < 0 || (last_capture_pts_ >= 0 && pts <= last_capture_pts_) ||
      now-probe_started_ > std::chrono::seconds(3)) { probe_failed_ = true; ++stats_.rejected; return; }
  last_capture_pts_ = pts;
  if (probes_ > 0 && now-last_probe_ < std::chrono::milliseconds(500)) return;
  if (probes_ >= 8 || !send_bind(std::to_string(pts))) { probe_failed_ = true; ++stats_.rejected; return; }
  probe_pts_.push_back(pts);
  ++probes_; last_probe_ = now;
}
ProtectionSession::Stats ProtectionSession::stats() const { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
std::int64_t ProtectionSession::phone_now() const {
  auto base = companion::decimal(state_.at("phone_time_us"));
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock_()-state_received_).count();
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
void ProtectionSession::clear_protection(bool reset_repetition) {
  stats_.unknown += pending_.size(); pending_.clear(); claims_.clear(); masks_.clear(); mask_pts_ = -1;
  native_stage_ = 0; emitted_stage_ = 0; requested_stage_ = 0; revision_ = 0;
  event_id_.clear(); event_screen_token_.clear(); unprotected_since_ = -1;
  policy_.reset_evidence(); if (reset_repetition) policy_.reset_repetition();
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
      if (!hello_.is_null() || message.at("session_id") == previous_session_) { ++stats_.rejected; return; }
      if (!previous_boot_.empty() && message.at("phone_boot_id") != previous_boot_) policy_.reset_repetition();
      previous_session_ = message.at("session_id"); previous_boot_ = message.at("phone_boot_id");
      unprotected_since_ = -1;
      first_state_ = true;
      probe_failed_ = false; probes_ = 0; last_capture_pts_ = -1; probe_pts_.clear(); clock_verified_ = false;
      probe_started_ = clock_();
      hello_ = message; bound_ = false; state_ = nullptr; receive_seq_ = 0; send_seq_ = 0;
      policy_.reset_evidence(); stats_.unknown += pending_.size(); pending_.clear();
      if (!send_bind(nullptr)) { probe_failed_ = true; ++stats_.rejected; } return;
    }
    if (hello_.is_null() || message.at("session_id") != hello_.at("session_id") || message.at("stream_id") != stream_id_) { ++stats_.rejected; return; }
    auto seq = companion::decimal(message.at("seq"),true);
    if (seq <= receive_seq_) { ++stats_.rejected; return; } receive_seq_ = seq;
    if (type == "bound") {
      auto request = companion::decimal(message.at("request_seq"),true);
      if (request > bind_seq_ || probe_failed_ || clock_()-probe_started_ > std::chrono::seconds(3)) { ++stats_.rejected; return; }
      if (message.at("status") == "rejected") { probe_failed_ = true; bound_ = false; clock_verified_ = false; }
      else if (message.at("status") == "accepted") {
        if (request < 4 || probes_ < 3 || request > static_cast<std::int64_t>(probe_pts_.size())+1 ||
            probe_pts_[request-2]-probe_pts_.front() < 1000000 ||
            clock_()-probe_started_ < std::chrono::seconds(1)) { ++stats_.rejected; return; }
        bound_ = true; clock_verified_ = true;
      }
      return;
    }
    if (!bound_) { ++stats_.rejected; return; }
    if (type == "state") {
      // Stale timestamps or policy rollback cannot refresh an execution gate.
      if (!state_.is_null() && companion::decimal(message.at("phone_time_us")) < companion::decimal(state_.at("phone_time_us"))) { ++stats_.rejected; return; }
      if (!message.at("policy").is_null()) {
        auto p = companion::profile_from_json(message.at("policy"));
        if (!policy_.set_profile(p.age,p.revision)) { ++stats_.rejected; return; }
      } else policy_.reset_evidence();
      if (!state_.is_null() && !state_.at("screen").is_null() && !message.at("screen").is_null()) {
        const auto& old = state_.at("screen"); const auto& next = message.at("screen");
        if (old.at("screen_token") == next.at("screen_token")) {
          for (auto key : {"content_epoch","display_id","width","height","rotation_deg","window_id","package","status","valid_from_us"})
            if (old.at(key) != next.at(key)) { ++stats_.rejected; bound_ = false; policy_.reset_evidence(); return; }
        }
      }
      state_ = message; state_received_ = clock_();
      const auto& protection = state_.at("protection");
      if (first_state_) { clear_protection(false); first_state_ = false; }
      if (protection.at("stage").get<int>() > 0) {
        unprotected_since_ = -1;
        if (event_id_ != protection.at("event_id")) clear_protection(false);
        event_id_ = protection.at("event_id"); event_screen_token_ = protection.at("target_screen_token");
        const auto native_revision = companion::decimal(protection.at("action_revision"),true);
        revision_ = std::max(revision_,native_revision);
        native_stage_ = std::max(native_stage_,protection.at("stage").get<int>());
        requested_stage_ = std::max(requested_stage_,native_stage_);
        std::vector<companion::Rect> rects;
        for (const auto& r : protection.at("covered_rects")) rects.push_back(companion::rect_from_json(r));
        for (auto it=claims_.begin();it!=claims_.end() && it->first<=native_revision;) it=claims_.erase(it);
        claims_[native_revision] = {native_stage_,std::move(rects),companion::decimal(protection.at("applied_at_us")),true};
        reconcile_protection();
      } else if ((!claims_.empty() || native_stage_ > 0) && !state_.at("screen").is_null() && state_.at("screen").at("status") == "verified") {
        auto sampled = companion::decimal(state_.at("screen").at("sampled_at_us"));
        auto phone = companion::decimal(state_.at("phone_time_us"));
        bool settled = sampled <= phone && phone-sampled <= 500000;
        for (const auto& entry : claims_) {
          const auto pts = entry.second.pts;
          settled &= sampled >= pts && sampled-pts >= companion::kTtlUs+1500000;
        }
        if (!settled) unprotected_since_ = -1;
        else if (unprotected_since_ < 0) unprotected_since_ = sampled;
        else if (sampled-unprotected_since_ >= 100000) {
          // Quiescent commands and two native zero-state snapshots; never infer release from pixels.
          clear_protection(false);
        }
      } else unprotected_since_ = -1;
      return;
    }
    if (type == "released") {
      if (message.at("event_id") != event_id_ ||
          (revision_ > 0 && companion::decimal(message.at("action_revision")) != revision_) ||
          (!event_screen_token_.empty() && message.at("previous_screen_token") != event_screen_token_)) {
        ++stats_.rejected; return;
      }
      clear_protection(false); return;
    }
    if (type != "ack") { ++stats_.rejected; return; }
    auto revision = companion::decimal(message.at("action_revision")); auto it = pending_.find(revision);
    if (it == pending_.end()) { ++stats_.rejected; return; }
    const auto& command = it->second.command;
    for (auto k : {"event_id","action_revision","screen_token","requested_stage"})
      if (message.at(k) != command.at(k)) { ++stats_.rejected; return; }
    if (message.at("request_seq") != command.at("seq")) { ++stats_.rejected; return; }
    if (!message.at("executed_at_us").is_null()) {
      auto executed = companion::decimal(message.at("executed_at_us"));
      auto now = phone_now();
      if (executed < companion::decimal(command.at("pts_us")) || (executed > now && executed-now > 50000)) { ++stats_.rejected; return; }
    }
    if (message.at("executed_stage") == 1 || message.at("executed_stage") == 2) {
      auto sw = command.at("transform").at("viewport_display_px").at("width").get<int>();
      auto sh = command.at("transform").at("viewport_display_px").at("height").get<int>();
      std::vector<companion::Rect> actual_rects;
      for (const auto& r : message.at("display_rects")) {
        auto rect = companion::rect_from_json(r);
        if (rect.x+rect.width > sw || rect.y+rect.height > sh) { ++stats_.rejected; return; }
        actual_rects.push_back(rect);
      }
      if (message.at("executed_stage") == 2) {
        if (actual_rects != std::vector<companion::Rect>{{0,0,sw,sh}}) { ++stats_.rejected; return; }
      } else if (message.at("status") == "executed" || message.at("status") == "duplicate") {
        for (const auto& r : command.at("regions")) {
          auto rect = companion::map_rect(companion::rect_from_json(r.at("crop_frame_px")),
            command.at("frame").at("width"),command.at("frame").at("height"),0,{0,0,sw,sh});
          if (std::find(actual_rects.begin(),actual_rects.end(),rect) == actual_rects.end()) { ++stats_.rejected; return; }
        }
      }
    }
    if (message.at("status") == "pending") return;
    auto actual = message.at("executed_stage").get<int>();
    if ((message.at("status") == "executed" || message.at("status") == "duplicate") &&
        actual == command.at("requested_stage").get<int>() && message.at("error").is_null()) {
      ++stats_.executed;
      policy_.executed(event_id_,command.at("package"),actual,companion::decimal(message.at("executed_at_us")));
    } else {
      ++stats_.failed;
      if (message.at("status") == "rejected" || message.at("status") == "failed") {
        if (actual == 0) {
          auto claim = claims_.find(revision);
          if (claim != claims_.end() && !claim->second.authoritative) claims_.erase(claim);
        }
        else {
          auto claim = claims_.find(revision);
          if (claim != claims_.end() && !claim->second.authoritative) {
            claim->second.stage = actual;
            if (!message.at("display_rects").empty()) {
              for (const auto& rect : message.at("display_rects")) {
                auto mapped = companion::rect_from_json(rect);
                if (std::find(claim->second.masks.begin(),claim->second.masks.end(),mapped) == claim->second.masks.end())
                  claim->second.masks.push_back(mapped);
              }
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
  auto now = clock_();
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
        state_.at("health").at("keystore") != "ready" || state_.at("health").at("pairing") != "paired" ||
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
    if (!event_screen_token_.empty() && event_screen_token_ != screen.at("screen_token")) { policy_.reset_evidence(); return; }
    auto decision = policy_.evaluate(batch,event_id_,now);
    if (decision.stage == 0 || decision.stage < emitted_stage_ || decision.stage < requested_stage_ || !pending_.empty()) return;
    if (decision.stage == emitted_stage_ && decision.stage > 1) return;
    if (event_id_.empty()) event_id_ = companion::uuid();
    if (revision_ == INT64_MAX || send_seq_ == INT64_MAX) { ++stats_.rejected; return; }
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
      requested_stage_ = std::max(requested_stage_,decision.stage);
      event_screen_token_ = command.at("screen_token");
      pending_[revision_] = {command,clock_()};
      claims_[revision_] = {decision.stage,std::move(rects),batch.pts_us};
      reconcile_protection();
    }
  } catch (const std::exception&) { ++stats_.rejected; policy_.reset_evidence(); }
}
}  // namespace k230::inspector
