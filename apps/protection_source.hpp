#pragma once

#include <array>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include <libavutil/hash.h>
}
#include "k230/bridge/scrcpy_session.hpp"

namespace k230 {
inline bool protection_source_verified(const bridge::ScrcpyConfig& config) {
  if (config.server_version != "4.0") return false;
  struct File {
    int fd;
    ~File() { if (fd >= 0) ::close(fd); }
  } file{::open(config.server_jar.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC)};
  struct stat info{};
  if (file.fd < 0 || ::fstat(file.fd,&info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 1048576) return false;
  AVHashContext* raw = nullptr;
  if (av_hash_alloc(&raw,"sha256") != 0) return false;
  auto free_hash = [](AVHashContext* hash) { av_hash_freep(&hash); };
  std::unique_ptr<AVHashContext,decltype(free_hash)> hash(raw,free_hash);
  av_hash_init(hash.get());
  std::array<std::uint8_t,8192> buffer{};
  std::size_t total = 0;
  for (;;) {
    auto count = ::read(file.fd,buffer.data(),buffer.size());
    if (count < 0) return false;
    if (count == 0) break;
    total += static_cast<std::size_t>(count);
    if (total > 1048576) return false;
    av_hash_update(hash.get(),buffer.data(),static_cast<std::size_t>(count));
  }
  std::array<std::uint8_t,65> digest{};
  av_hash_final_hex(hash.get(),digest.data(),digest.size());
  return std::string(reinterpret_cast<const char*>(digest.data())) ==
    "84924bd564a1eb6089c872c7521f968058977f91f5ff02514a8c74aff3210f3a";
}
}  // namespace k230
