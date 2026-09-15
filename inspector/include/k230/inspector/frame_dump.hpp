#pragma once

#include <string>

#include "k230/inspector/frame.hpp"

namespace k230::inspector {

// Writes the frame as a binary PPM (P6). Debug aid: no image library needed,
// opens in any viewer, and `ffmpeg -i x.ppm x.png` converts if required.
bool write_ppm(const VideoFrame& frame, const std::string& path);

// Writes interleaved s16 PCM as a WAV file.
bool write_wav(const std::vector<std::int16_t>& samples, std::uint32_t sample_rate, std::uint16_t channels,
               const std::string& path);

}  // namespace k230::inspector
