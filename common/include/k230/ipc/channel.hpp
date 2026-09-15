#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

#include "k230/media_packet.hpp"
#include "k230/verdict.hpp"

namespace k230::ipc {

// Little core -> big core: compressed media packets.
// Big core -> little core: verdicts.
//
// Two implementations exist:
//   InProcessQueue<T>   both "cores" in one PC process (development / tests)
//   DataFifo / IpcMsg   K230 SDK shared-memory transports (see datafifo_channel.hpp)
//
// Producers must never block on a full queue in the media path: dropping the
// oldest non-config packet is preferable to stalling the USB reader thread.
template <typename T>
class Sink {
 public:
  virtual ~Sink() = default;
  // Returns false if the item was dropped.
  virtual bool push(T&& item) = 0;
  virtual void close() = 0;
};

template <typename T>
class Source {
 public:
  virtual ~Source() = default;
  // Blocks up to `timeout`; returns nullopt on timeout or when closed & empty.
  virtual std::optional<T> pop(std::chrono::milliseconds timeout) = 0;
  virtual bool closed() const = 0;
};

using PacketSink = Sink<MediaPacket>;
using PacketSource = Source<MediaPacket>;
using VerdictSink = Sink<Verdict>;
using VerdictSource = Source<Verdict>;

// Bounded MPMC queue. When full, the oldest droppable item is evicted so that
// the newest data always gets through (low latency beats completeness for a
// live monitor). Items for which `keep(item)` returns true are never evicted;
// that is used to protect codec config packets (SPS/PPS) which are required
// to bootstrap the decoder.
template <typename T>
class InProcessQueue final : public Sink<T>, public Source<T> {
 public:
  using KeepPredicate = bool (*)(const T&);

  explicit InProcessQueue(std::size_t capacity, KeepPredicate keep = nullptr)
      : capacity_(capacity), keep_(keep) {}

  bool push(T&& item) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    bool dropped = false;
    if (items_.size() >= capacity_) {
      auto victim = items_.end();
      for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (!keep_ || !keep_(*it)) {
          victim = it;
          break;
        }
      }
      if (victim == items_.end()) {
        // Only protected items in the queue: grow instead of losing them.
      } else {
        items_.erase(victim);
        ++drops_;
        dropped = true;
      }
    }
    items_.push_back(std::move(item));
    cv_.notify_one();
    return !dropped;
  }

  std::optional<T> pop(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return closed_ || !items_.empty(); });
    if (items_.empty()) return std::nullopt;
    T item = std::move(items_.front());
    items_.pop_front();
    return item;
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.notify_all();
  }

  bool closed() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

  std::size_t drops() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drops_;
  }

 private:
  const std::size_t capacity_;
  const KeepPredicate keep_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<T> items_;
  std::size_t drops_ = 0;
  bool closed_ = false;
};

inline bool keep_config_packets(const MediaPacket& p) { return p.is_config; }

}  // namespace k230::ipc
