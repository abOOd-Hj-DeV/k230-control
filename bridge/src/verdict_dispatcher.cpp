#include "k230/bridge/verdict_dispatcher.hpp"

#include "k230/log.hpp"

namespace k230::bridge {

namespace {
constexpr const char* kTag = "verdict";
}

VerdictDispatcher::VerdictDispatcher(CompanionConfig config, AdbController adb, std::string serial,
                                     std::shared_ptr<ipc::VerdictSource> source)
    : config_(std::move(config)), adb_(std::move(adb)), serial_(std::move(serial)), source_(std::move(source)) {}

VerdictDispatcher::~VerdictDispatcher() { stop(); }

bool VerdictDispatcher::start() {
  if (!config_.local_only && !serial_.empty() &&
      !adb_.forward(serial_, config_.local_port, "localabstract:" + config_.abstract_socket)) {
    K230_LOG_WARN(kTag) << "could not forward companion socket; verdicts will only be logged";
  }
  stop_ = false;
  thread_ = std::thread([this] { loop(); });
  return true;
}

void VerdictDispatcher::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  socket_.close();
  if (!config_.local_only && !serial_.empty()) {
    adb_.forward_remove(serial_, config_.local_port);
    serial_.clear();
  }
}

bool VerdictDispatcher::ensure_connected() {
  if (socket_.connected()) return true;
  const auto now = std::chrono::steady_clock::now();
  if (now < next_connect_attempt_) return false;
  next_connect_attempt_ = now + std::chrono::milliseconds(config_.reconnect_delay_ms);
  if (socket_.connect("127.0.0.1", config_.local_port, 500)) {
    // adb accepts the TCP connection even when no app is listening on the
    // phone; the companion app must greet us so we can tell the difference.
    std::uint8_t hello = 0;
    if (socket_.recv_exact(&hello, 1, 500) && hello == 'K' &&
        socket_.set_send_timeout(config_.send_timeout_ms)) {
      K230_LOG_INFO(kTag) << "companion app connected";
      return true;
    }
    socket_.close();
  }
  return false;
}

void VerdictDispatcher::loop() {
  while (!stop_) {
    auto verdict = source_->pop(std::chrono::milliseconds(200));
    if (!verdict) {
      if (source_->closed()) break;
      continue;
    }
    if (observer_) observer_(*verdict);
    if (config_.local_only || verdict->action < config_.min_action) continue;

    const std::string line = to_json_line(*verdict);
    if (ensure_connected() && socket_.send_all(line)) {
      ++sent_;
    } else {
      socket_.close();
      ++dropped_;
      K230_LOG_WARN(kTag) << "companion unreachable, dropped: " << line.substr(0, line.size() - 1);
    }
  }
}

}  // namespace k230::bridge
