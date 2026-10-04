#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include "k230/inspector/age_policy.hpp"
#include "k230/ipc/channel.hpp"

namespace k230::inspector {
class ProtectionSession {
 public:
  struct Stats { std::uint64_t submitted = 0, executed = 0, failed = 0, unknown = 0, expired = 0, rejected = 0; };
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  ProtectionSession(std::shared_ptr<ipc::ControlSink> downstream, bool diagnostic_pts_clock_assertion,
                    Clock clock = [] { return std::chrono::steady_clock::now(); },
                    std::string capture_source = "scrcpy-4.0-display");
  void receive(const companion::Json& message);
  void disconnected();
  void observe_capture_pts(std::int64_t pts_us);
  void analyze(AnalysisBatch batch);
  Stats stats() const;
  const std::string& stream_id() const { return stream_id_; }
 private:
  std::int64_t phone_now() const;
  companion::Json make_decision(const AnalysisBatch&, const AgeDecision&, const std::vector<RegionProof>&);
  void expire_pending();
  void reconcile_protection();
  void clear_protection(bool reset_repetition);
  bool send_bind(const companion::Json& capture_pts);
  std::shared_ptr<ipc::ControlSink> downstream_;
  Clock clock_;
  std::string capture_source_;
  bool clock_verified_ = false, bound_ = false;
  std::string stream_id_, event_id_, event_screen_token_;
  std::string previous_session_, previous_boot_;
  bool first_state_ = true;
  bool probe_failed_ = false;
  std::int64_t last_capture_pts_ = -1;
  std::vector<std::int64_t> probe_pts_;
  unsigned probes_ = 0;
  std::chrono::steady_clock::time_point probe_started_, last_probe_;
  std::int64_t unprotected_since_ = -1;
  std::int64_t send_seq_ = 0, receive_seq_ = 0, revision_ = 0, bind_seq_ = 0;
  companion::Json hello_, state_;
  std::chrono::steady_clock::time_point state_received_;
  AgePolicy policy_;
  struct Pending { companion::Json command; std::chrono::steady_clock::time_point submitted; };
  std::map<std::int64_t, Pending> pending_;
  struct ProtectionClaim { int stage; std::vector<companion::Rect> masks; std::int64_t pts; bool authoritative = false; };
  std::map<std::int64_t, ProtectionClaim> claims_;
  std::vector<companion::Rect> masks_;
  std::int64_t mask_pts_ = -1;
  int emitted_stage_ = 0;
  int native_stage_ = 0;
  int requested_stage_ = 0;
  Stats stats_;
  mutable std::mutex mutex_;
};
}  // namespace k230::inspector
