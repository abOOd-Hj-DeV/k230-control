#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include "k230/bridge/verdict_dispatcher.hpp"

namespace k230::bridge {
class CompanionDispatcher {
 public:
  using Receiver = std::function<void(const companion::Json&)>;
  CompanionDispatcher(CompanionConfig config, AdbController adb, std::string serial,
    std::shared_ptr<ipc::ControlSource> downstream, Receiver receiver, std::function<void()> disconnect);
  ~CompanionDispatcher();
  bool start();
  void stop();
  std::uint64_t sent() const { return sent_; }
  std::uint64_t dropped() const { return dropped_; }
 private:
  bool connect();
  void loop();
  void close();
  CompanionConfig config_;
  AdbController adb_;
  std::string serial_;
  std::shared_ptr<ipc::ControlSource> downstream_;
  Receiver receiver_;
  std::function<void()> disconnect_;
  TcpSocket socket_;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> sent_{0}, dropped_{0};
  companion::LineAssembler assembler_;
  companion::Json hello_, state_;
  std::string stream_;
  std::int64_t last_send_seq_ = 0, last_receive_seq_ = 0;
  bool bound_ = false;
  std::chrono::steady_clock::time_point next_connect_{}, state_at_{};
};
}  // namespace k230::bridge
