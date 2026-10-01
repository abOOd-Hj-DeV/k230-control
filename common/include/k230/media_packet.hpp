#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace k230 {

enum class StreamType : std::uint8_t { Video = 0, Audio = 1 };

// Codec identifiers as sent by scrcpy-server (4 ASCII bytes, big-endian u32).
enum class CodecId : std::uint32_t {
  Unknown = 0,
  H264 = 0x68323634,  // "h264"
  H265 = 0x68323635,  // "h265"
  AV1 = 0x00617631,   // "\0av1"
  Opus = 0x6f707573,  // "opus"
  Aac = 0x00616163,   // "\0aac"
  Flac = 0x666c6163,  // "flac"
  Raw = 0x00726177,   // "\0raw"
};

const char* to_string(CodecId id);
const char* to_string(StreamType type);

constexpr std::int64_t kNoPts = -1;

// One compressed access unit (video) or one audio packet, as transported
// from the little core to the big core. The payload is exactly the bytes
// produced by MediaCodec on the phone (Annex-B for H.264/H.265, raw PCM
// s16le for "raw" audio).
struct MediaPacket {
  StreamType stream = StreamType::Video;
  CodecId codec = CodecId::Unknown;
  std::int64_t pts_us = kNoPts;  // phone monotonic clock, microseconds
  bool is_config = false;        // SPS/PPS (video) or codec header (audio)
  bool is_key_frame = false;
  std::uint32_t seq = 0;  // per-stream counter; a gap means packets were dropped
  std::vector<std::uint8_t> data;
};

struct VideoSessionInfo {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool client_resized = false;
};

struct AudioFormat {
  std::uint32_t sample_rate = 48000;
  std::uint16_t channels = 2;
  std::uint16_t bits_per_sample = 16;
};

}  // namespace k230
