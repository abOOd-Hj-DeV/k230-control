#pragma once

#include <cstdio>
#include <mutex>
#include <optional>
#include <string>

#include "k230/media_packet.hpp"

namespace k230 {

// Dump of the packet stream leaving the bridge, in ipc::wire encoding, with
// an 8-byte file magic. Lets the inspector be developed and unit-tested on a
// PC without a phone attached, and lets a stream captured on the K230 be
// replayed on a PC for debugging.
class RecordingWriter {
 public:
  static constexpr char kMagic[8] = {'K', '2', '3', '0', 'R', 'E', 'C', '1'};

  explicit RecordingWriter(const std::string& path);
  ~RecordingWriter();
  RecordingWriter(const RecordingWriter&) = delete;
  RecordingWriter& operator=(const RecordingWriter&) = delete;

  bool ok() const { return file_ != nullptr; }
  bool write(const MediaPacket& packet);

 private:
  std::FILE* file_ = nullptr;
  std::mutex mutex_;
};

class RecordingReader {
 public:
  explicit RecordingReader(const std::string& path);
  ~RecordingReader();
  RecordingReader(const RecordingReader&) = delete;
  RecordingReader& operator=(const RecordingReader&) = delete;

  bool ok() const { return file_ != nullptr; }
  const std::string& error() const { return error_; }
  std::optional<MediaPacket> next();

 private:
  std::FILE* file_ = nullptr;
  std::string error_;
};

}  // namespace k230
