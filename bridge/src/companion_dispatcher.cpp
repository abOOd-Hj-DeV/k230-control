#include "k230/bridge/companion_dispatcher.hpp"
#include "k230/log.hpp"
#include <array>

namespace k230::bridge {
namespace {
std::int64_t monotonic_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
CompanionDispatcher::CompanionDispatcher(CompanionConfig config, AdbController adb, std::string serial,
    std::shared_ptr<ipc::ControlSource> downstream, Receiver receiver, std::function<void()> disconnect)
  : config_(std::move(config)), adb_(std::move(adb)), serial_(std::move(serial)), downstream_(std::move(downstream)),
    receiver_(std::move(receiver)), disconnect_(std::move(disconnect)) {}
CompanionDispatcher::~CompanionDispatcher() { stop(); }
bool CompanionDispatcher::start() {
  if (!serial_.empty() && !adb_.forward(serial_,config_.local_port,"localabstract:"+config_.abstract_socket))
    K230_LOG_WARN("companion-v2") << "forward unavailable; local media remains active";
  stopping_ = false; worker_ = std::thread(&CompanionDispatcher::loop,this); return true;
}
void CompanionDispatcher::stop() {
  stopping_ = true;
  socket_.shutdown();
  // Every connect/read/send has a <=500ms absolute bound. No media joins this worker.
  if (worker_.joinable()) worker_.join();
  socket_.close();
  if (!serial_.empty()) { adb_.forward_remove(serial_,config_.local_port); serial_.clear(); }
}
void CompanionDispatcher::close() {
  socket_.close(); bound_ = false; hello_ = nullptr; state_ = nullptr; stream_.clear();
  assembler_ = {}; last_send_seq_ = 0; last_receive_seq_ = 0;
  if (disconnect_) disconnect_();
}
bool CompanionDispatcher::connect() {
  auto now = std::chrono::steady_clock::now();
  if (now < next_connect_) return false;
  next_connect_ = now+std::chrono::milliseconds(config_.reconnect_delay_ms);
  if (!socket_.connect("127.0.0.1",config_.local_port,500)) return false;
  std::uint8_t greeting = 0;
  if (stopping_ || !socket_.recv_exact(&greeting,1,500) || greeting != 'K' || !socket_.set_send_timeout(500)) { close(); return false; }
  auto deadline = std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
  try {
    // Read exactly through hello LF, without consuming a later state record.
    while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
      std::uint8_t byte = 0;
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
      if (left <= 0 || socket_.recv(&byte,1,static_cast<int>(left)) != 1) break;
      auto messages = assembler_.feed(&byte,1,monotonic_us());
      if (!messages.empty()) {
        if (messages.front().at("type") != "hello") break;
        hello_ = messages.front(); receiver_(hello_); return true;
      }
    }
  } catch (const std::exception&) {}
  // K-only/unsupported companions are diagnostics-only, never a v1 intervention fallback.
  close(); K230_LOG_WARN("companion-v2") << "v2 negotiation unavailable"; return false;
}
void CompanionDispatcher::loop() {
  std::deque<std::int64_t> decisions;
  while (!stopping_) {
    if (!socket_.connected()) {
      connect();
      if (!socket_.connected()) {
        auto item = downstream_->pop(std::chrono::milliseconds(20));
        if (item) ++dropped_;
        if (downstream_->closed()) break;
        continue;
      }
    }
    try {
      std::array<std::uint8_t,4096> bytes{};
      auto count = socket_.recv(bytes.data(),bytes.size(),20);
      assembler_.check_deadline(monotonic_us());
      if (count == 0 || (count < 0 && !socket_.last_receive_timed_out())) { close(); continue; }
      if (count > 0) for (const auto& message : assembler_.feed(bytes.data(),count,monotonic_us())) {
        if (message.at("type") == "hello" || message.at("type") == "bind" || message.at("type") == "decision" ||
            message.at("session_id") != hello_.at("session_id") || message.at("stream_id") != stream_) throw std::runtime_error("session_mismatch");
        auto seq = companion::decimal(message.at("seq"));
        if (seq <= last_receive_seq_) throw std::runtime_error("stale");
        last_receive_seq_ = seq;
        if (message.at("type") == "bound") bound_ = message.at("status") == "accepted";
        if (message.at("type") == "state") { state_ = message; state_at_ = std::chrono::steady_clock::now(); }
        receiver_(message);
      }
      auto item = downstream_->pop(std::chrono::milliseconds(0));
      if (!item) { if (downstream_->closed()) break; continue; }
      auto message = std::move(*item);
      if (message.at("session_id") != hello_.at("session_id")) { ++dropped_; continue; }
      auto seq = companion::decimal(message.at("seq"));
      if (seq <= last_send_seq_) { ++dropped_; continue; }
      if (message.at("type") == "bind") {
        if (last_send_seq_ != 0) { ++dropped_; continue; } stream_ = message.at("stream_id");
      } else if (message.at("type") == "decision") {
        if (!bound_ || state_.is_null() || message.at("stream_id") != stream_) { ++dropped_; continue; }
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-state_at_).count();
        auto base = companion::decimal(state_.at("phone_time_us"));
        if (base > INT64_MAX-elapsed) throw std::runtime_error("bounds");
        companion::validate_decision(message,state_,base+elapsed,true);
        auto now = monotonic_us();
        while (!decisions.empty() && now-decisions.front() >= 1000000) decisions.pop_front();
        if (decisions.size() >= 10) { ++dropped_; continue; } decisions.push_back(now);
      } else { ++dropped_; continue; }
      if (!socket_.send_all(companion::line(message))) { ++dropped_; close(); continue; }
      last_send_seq_ = seq; ++sent_;
    } catch (const std::exception&) { ++dropped_; close(); }
  }
  close();
}
}  // namespace k230::bridge
