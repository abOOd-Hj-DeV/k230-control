#pragma once

#include <chrono>
#include <mutex>
#include "k230/inspector/age_policy.hpp"
#include "k230/ipc/channel.hpp"

namespace k230::inspector {
class ProtectionSession {
 public:
  struct Stats { std::uint64_t submitted = 0, executed = 0, failed = 0, unknown = 0, expired = 0, rejected = 0; };
  ProtectionSession(std::shared_ptr<ipc::ControlSink> downstream, bool verified_pts_clock);
  void receive(const companion::Json& message);
  void disconnected();
  void analyze(AnalysisBatch batch);
  Stats stats() const;
  const std::string& stream_id() const { return stream_id_; }
 private:
  std::int64_t phone_now() const;
  companion::Json make_decision(const AnalysisBatch&, const AgeDecision&, const std::vector<RegionProof>&);
  void expire_pending();
  void reconcile_protection();
  std::shared_ptr<ipc::ControlSink> downstream_;
  bool clock_verified_ = false, bound_ = false;
  std::string stream_id_, event_id_, event_screen_token_;
  std::int64_t send_seq_ = 0, receive_seq_ = 0, revision_ = 0, bind_seq_ = 0;
  companion::Json hello_, state_;
  std::chrono::steady_clock::time_point state_received_;
  AgePolicy policy_;
  struct Pending { companion::Json command; std::chrono::steady_clock::time_point submitted; };
  std::map<std::int64_t, Pending> pending_;
  struct ProtectionClaim { int stage; std::vector<companion::Rect> masks; std::int64_t pts; };
  std::map<std::int64_t, ProtectionClaim> claims_;
  std::vector<companion::Rect> masks_;
  std::int64_t mask_pts_ = -1;
  int emitted_stage_ = 0;
  int native_stage_ = 0;
  Stats stats_;
  mutable std::mutex mutex_;
};
}  // namespace k230::inspector
