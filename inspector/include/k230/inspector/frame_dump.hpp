#pragma once

#include <string>

#include "k230/inspector/frame.hpp"
#include "k230/inspector/pcm_ring.hpp"

namespace k230::inspector {

bool write_png(const VideoFrame& frame, const std::string& path);

// Writes interleaved s16 PCM as a WAV file.
bool write_wav(const std::vector<std::int16_t>& samples, std::uint32_t sample_rate, std::uint16_t channels,
               const std::string& path);

class TestCapture {
 public:
  static constexpr std::int64_t kDurationUs = 8'000'000;
  explicit TestCapture(std::string directory);
  void push_audio(const PcmChunk& chunk);
  void push_frame(const VideoFrame& frame);
  bool finish();
  std::uint32_t images_written() const { return images_written_; }
  std::int64_t audio_filled_us() const { return audio_filled_us_; }

 private:
  std::string directory_;
  PcmRing audio_{kDurationUs + 2'000'000};
  std::int64_t start_pts_us_ = -1;
  std::int64_t audio_filled_us_ = 0;
  std::uint32_t images_written_ = 0;
  std::uint32_t next_second_ = 0;
  bool failed_ = false;
};

}  // namespace k230::inspector
