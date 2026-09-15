#include "k230/recording.hpp"

#include <cstring>

#include "k230/ipc/wire.hpp"

namespace k230 {

constexpr char RecordingWriter::kMagic[8];

RecordingWriter::RecordingWriter(const std::string& path) {
  file_ = std::fopen(path.c_str(), "wb");
  if (file_ && std::fwrite(kMagic, 1, sizeof(kMagic), file_) != sizeof(kMagic)) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

RecordingWriter::~RecordingWriter() {
  if (file_) std::fclose(file_);
}

bool RecordingWriter::write(const MediaPacket& packet) {
  if (!file_) return false;
  std::uint8_t header[ipc::kPacketHeaderSize];
  ipc::encode_header(packet, header);
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::fwrite(header, 1, sizeof(header), file_) != sizeof(header)) return false;
  if (!packet.data.empty() &&
      std::fwrite(packet.data.data(), 1, packet.data.size(), file_) != packet.data.size()) {
    return false;
  }
  return true;
}

RecordingReader::RecordingReader(const std::string& path) {
  file_ = std::fopen(path.c_str(), "rb");
  if (!file_) {
    error_ = "cannot open " + path;
    return;
  }
  char magic[8];
  if (std::fread(magic, 1, sizeof(magic), file_) != sizeof(magic) ||
      std::memcmp(magic, RecordingWriter::kMagic, sizeof(magic)) != 0) {
    error_ = "not a K230REC1 file: " + path;
    std::fclose(file_);
    file_ = nullptr;
  }
}

RecordingReader::~RecordingReader() {
  if (file_) std::fclose(file_);
}

std::optional<MediaPacket> RecordingReader::next() {
  if (!file_) return std::nullopt;
  std::uint8_t header[ipc::kPacketHeaderSize];
  if (std::fread(header, 1, sizeof(header), file_) != sizeof(header)) return std::nullopt;
  MediaPacket packet;
  auto len = ipc::decode_header(header, packet);
  if (!len) {
    error_ = "corrupt packet header";
    return std::nullopt;
  }
  packet.data.resize(*len);
  if (*len > 0 && std::fread(packet.data.data(), 1, *len, file_) != *len) {
    error_ = "truncated packet payload";
    return std::nullopt;
  }
  return packet;
}

}  // namespace k230
