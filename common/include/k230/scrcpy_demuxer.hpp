#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "k230/media_packet.hpp"

namespace k230 {

// Incremental parser for one scrcpy-server media socket (video or audio).
//
// Wire format produced by scrcpy-server >= 2.0 with the options we use
// (send_frame_meta=true, send_codec_meta=true):
//
//   [1 byte dummy]            only on the first socket, if send_dummy_byte
//   [64 bytes device name]    only on the first socket, if send_device_meta
//   [4 bytes codec id]        big-endian ASCII, e.g. "h264", "opus", "\0raw"
//   repeated:
//     [12 bytes header] [payload]
//
// Header layout (all big-endian):
//   bytes 0..7   pts_flags  bit63 = session packet (video only)
//                           bit62 = config packet (no PTS)
//                           bit61 = key frame
//                           bits 0..60 = PTS in microseconds
//   bytes 8..11  payload length
//
// A session packet carries no payload: bytes 4..7 = width, 8..11 = height,
// byte 3 bit0 = client_resized. It is sent at the start of the video stream
// and again every time the encoder restarts (rotation, resize).
//
// The parser is fed arbitrary byte chunks (as read from a socket) and emits
// fully reassembled packets, so callers never have to care about TCP
// segment boundaries.
class ScrcpyDemuxer {
 public:
  struct Options {
    StreamType stream = StreamType::Video;
    bool expect_dummy_byte = false;
    bool expect_device_name = false;
    bool expect_codec_id = true;
    std::size_t max_packet_size = 16u * 1024u * 1024u;
  };

  struct Callbacks {
    std::function<void(const std::string&)> on_device_name;
    std::function<void(CodecId)> on_codec;
    std::function<void(const VideoSessionInfo&)> on_session;
    std::function<void(MediaPacket&&)> on_packet;
  };

  struct Stats {
    std::uint64_t packets = 0;
    std::uint64_t config_packets = 0;
    std::uint64_t key_frames = 0;
    std::uint64_t sessions = 0;
    std::uint64_t bytes = 0;
  };

  static constexpr std::size_t kHeaderSize = 12;
  static constexpr std::size_t kDeviceNameSize = 64;
  static constexpr std::uint64_t kFlagSession = std::uint64_t{1} << 63;
  static constexpr std::uint64_t kFlagConfig = std::uint64_t{1} << 62;
  static constexpr std::uint64_t kFlagKeyFrame = std::uint64_t{1} << 61;
  static constexpr std::uint64_t kPtsMask = kFlagKeyFrame - 1;

  ScrcpyDemuxer(Options options, Callbacks callbacks);

  // Consume a chunk of bytes. Callbacks fire synchronously for every complete
  // item found. Returns false once the stream is in an unrecoverable state.
  bool feed(const std::uint8_t* data, std::size_t len);
  bool feed(const std::vector<std::uint8_t>& chunk) { return feed(chunk.data(), chunk.size()); }

  bool failed() const { return failed_; }
  const std::string& error() const { return error_; }
  CodecId codec() const { return codec_; }
  const Stats& stats() const { return stats_; }

 private:
  enum class State { DummyByte, DeviceName, CodecId, Header, Payload, Failed };

  bool step();  // returns true if progress was made
  void fail(std::string why);
  std::size_t available() const { return buffer_.size() - read_pos_; }
  const std::uint8_t* cursor() const { return buffer_.data() + read_pos_; }
  void consume(std::size_t n);
  void compact();

  Options options_;
  Callbacks callbacks_;
  State state_;
  std::vector<std::uint8_t> buffer_;
  std::size_t read_pos_ = 0;

  CodecId codec_ = CodecId::Unknown;
  std::uint64_t pending_pts_flags_ = 0;
  std::uint32_t pending_len_ = 0;

  Stats stats_;
  bool failed_ = false;
  std::string error_;
};

}  // namespace k230
