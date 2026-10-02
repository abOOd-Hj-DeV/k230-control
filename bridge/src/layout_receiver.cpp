#include "k230/bridge/layout_receiver.hpp"

#include <array>
#include <chrono>
#include <fstream>

#include "k230/bridge/tcp_socket.hpp"
#include "k230/log.hpp"

namespace k230::bridge {
namespace {
bool receive(TcpSocket& socket, std::uint8_t* data, std::size_t size, const std::atomic<bool>& stop) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  std::size_t done = 0;
  while (!stop && done < size) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) return false;
    const long got = socket.recv(data + done, size - done, static_cast<int>(remaining));
    if (got <= 0) return false;
    done += static_cast<std::size_t>(got);
  }
  return done == size;
}
}
LayoutReceiver::LayoutReceiver(LayoutReceiverConfig config, AdbController adb, std::string serial,
                               std::shared_ptr<LayoutCache> cache)
    : config_(std::move(config)), adb_(std::move(adb)), serial_(std::move(serial)), cache_(std::move(cache)) {}
LayoutReceiver::~LayoutReceiver() { stop(); }
void LayoutReceiver::start() { thread_ = std::thread(&LayoutReceiver::run, this); }
void LayoutReceiver::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  cache_->clear();
}

void LayoutReceiver::run() {
  std::ofstream recording;
  if (!config_.recording.empty()) {
    recording.open(config_.recording, std::ios::binary);
    if (!recording) K230_LOG_ERROR("layout") << "cannot open layout recording " << config_.recording;
  }
  while (!stop_) {
    if (!adb_.forward(serial_, config_.port, "localabstract:" + config_.socket)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    TcpSocket socket;
    if (!socket.connect("127.0.0.1", config_.port, 200)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    K230_LOG_INFO("layout") << "connected " << config_.socket;
    while (!stop_) {
      std::array<std::uint8_t, 4> prefix{};
      if (!receive(socket, prefix.data(), prefix.size(), stop_)) break;
      const auto size = layout_payload_size(prefix.data());
      if (size < kLayoutHeaderSize || size > kLayoutMaxPayload) break;
      std::vector<std::uint8_t> payload(size);
      if (!receive(socket, payload.data(), payload.size(), stop_)) break;
      auto snapshot = decode_layout(payload.data(), payload.size());
      if (!snapshot) {
        K230_LOG_WARN("layout") << "malformed snapshot; discarding connection";
        break;
      }
      if (!cache_->push(*snapshot)) continue;
      if (recording) {
        recording.write(reinterpret_cast<const char*>(prefix.data()), prefix.size());
        recording.write(reinterpret_cast<const char*>(payload.data()), payload.size());
      }
      if (observer_) observer_(*snapshot);
    }
    cache_->clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  adb_.forward_remove(serial_, config_.port);
}
}  // namespace k230::bridge
