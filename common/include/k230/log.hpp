#pragma once

#include <chrono>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>

namespace k230::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline Level& threshold() {
  static Level level = Level::Info;
  return level;
}

inline void write(Level level, const char* tag, const std::string& msg) {
  if (level < threshold()) return;
  static std::mutex mutex;
  static const auto t0 = std::chrono::steady_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  static const char* names[] = {"DBG", "INF", "WRN", "ERR"};
  std::lock_guard<std::mutex> lock(mutex);
  std::fprintf(stderr, "[%8lld.%03lld] %s %-9s %s\n", static_cast<long long>(ms / 1000),
               static_cast<long long>(ms % 1000), names[static_cast<int>(level)], tag, msg.c_str());
}

class Line {
 public:
  Line(Level level, const char* tag) : level_(level), tag_(tag) {}
  ~Line() { write(level_, tag_, stream_.str()); }
  template <typename T>
  Line& operator<<(const T& v) {
    stream_ << v;
    return *this;
  }

 private:
  Level level_;
  const char* tag_;
  std::ostringstream stream_;
};

}  // namespace k230::log

#define K230_LOG_DEBUG(tag) ::k230::log::Line(::k230::log::Level::Debug, tag)
#define K230_LOG_INFO(tag) ::k230::log::Line(::k230::log::Level::Info, tag)
#define K230_LOG_WARN(tag) ::k230::log::Line(::k230::log::Level::Warn, tag)
#define K230_LOG_ERROR(tag) ::k230::log::Line(::k230::log::Level::Error, tag)
