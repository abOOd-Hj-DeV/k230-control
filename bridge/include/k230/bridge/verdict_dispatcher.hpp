#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "k230/bridge/adb_controller.hpp"
#include "k230/bridge/tcp_socket.hpp"
#include "k230/ipc/channel.hpp"
#include "k230/verdict.hpp"

namespace k230::bridge {

struct CompanionConfig {
  // The companion Android app listens on this abstract unix socket; the bridge
  // reaches it through `adb forward tcp:<local_port> localabstract:<name>`.
  std::string abstract_socket = "k230_companion";
  std::uint16_t local_port = 27185;
  int reconnect_delay_ms = 1000;
  // Verdicts with action < min_action are logged locally but not sent.
  Action min_action = Action::Warn;
};

// Consumes verdicts from the big core and forwards them, as JSON lines, to the
// companion app on the phone. Loss of the phone connection never blocks the
// media path: verdicts are dropped with a warning while reconnecting.
class VerdictDispatcher {
 public:
  using Observer = std::function<void(const Verdict&)>;

  VerdictDispatcher(CompanionConfig config, AdbController adb, std::string serial,
                    std::shared_ptr<ipc::VerdictSource> source);
  ~VerdictDispatcher();

  void set_observer(Observer observer) { observer_ = std::move(observer); }
  bool start();
  void stop();

  std::uint64_t sent() const { return sent_; }
  std::uint64_t dropped() const { return dropped_; }

 private:
  void loop();
  bool ensure_connected();

  CompanionConfig config_;
  AdbController adb_;
  std::string serial_;
  std::shared_ptr<ipc::VerdictSource> source_;
  Observer observer_;
  TcpSocket socket_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> sent_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::chrono::steady_clock::time_point next_connect_attempt_{};
};

}  // namespace k230::bridge
