#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "k230/bridge/adb_controller.hpp"
#include "k230/layout.hpp"

namespace k230::bridge {
struct LayoutReceiverConfig {
  std::uint16_t port = 27186;
  std::string socket = "k230_layout";
  std::string recording;
};

class LayoutReceiver {
 public:
  using Observer = std::function<void(const LayoutSnapshot&)>;
  LayoutReceiver(LayoutReceiverConfig config, AdbController adb, std::string serial,
                 std::shared_ptr<LayoutCache> cache);
  ~LayoutReceiver();
  void start();
  void stop();
  void set_observer(Observer observer) { observer_ = std::move(observer); }

 private:
  void run();
  LayoutReceiverConfig config_;
  AdbController adb_;
  std::string serial_;
  std::shared_ptr<LayoutCache> cache_;
  Observer observer_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
}  // namespace k230::bridge
