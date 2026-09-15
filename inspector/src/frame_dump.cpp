#include "k230/inspector/frame_dump.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace k230::inspector {

namespace {

std::uint8_t clamp8(int v) { return static_cast<std::uint8_t>(std::max(0, std::min(255, v))); }

void put_le16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}

void put_le32(std::uint8_t* p, std::uint32_t v) {
  put_le16(p, static_cast<std::uint16_t>(v));
  put_le16(p + 2, static_cast<std::uint16_t>(v >> 16));
}

}  // namespace

bool write_ppm(const VideoFrame& frame, const std::string& path) {
  if (frame.width == 0 || frame.height == 0 || frame.data.size() < frame.luma_size() * 3 / 2) return false;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%u %u\n255\n", frame.width, frame.height);

  std::vector<std::uint8_t> row(static_cast<std::size_t>(frame.width) * 3);
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      const int Y = frame.y()[y * frame.width + x];
      const int Cb = frame.cb(x / 2, y / 2) - 128;
      const int Cr = frame.cr(x / 2, y / 2) - 128;
      // BT.601 limited range, integer approximation.
      const int c = 298 * (Y - 16);
      row[x * 3 + 0] = clamp8((c + 409 * Cr + 128) >> 8);
      row[x * 3 + 1] = clamp8((c - 100 * Cb - 208 * Cr + 128) >> 8);
      row[x * 3 + 2] = clamp8((c + 516 * Cb + 128) >> 8);
    }
    if (std::fwrite(row.data(), 1, row.size(), f) != row.size()) {
      std::fclose(f);
      return false;
    }
  }
  return std::fclose(f) == 0;
}

bool write_wav(const std::vector<std::int16_t>& samples, std::uint32_t sample_rate, std::uint16_t channels,
               const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const std::uint32_t data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
  std::uint8_t header[44];
  std::memcpy(header, "RIFF", 4);
  put_le32(header + 4, 36 + data_bytes);
  std::memcpy(header + 8, "WAVEfmt ", 8);
  put_le32(header + 16, 16);
  put_le16(header + 20, 1);  // PCM
  put_le16(header + 22, channels);
  put_le32(header + 24, sample_rate);
  put_le32(header + 28, sample_rate * channels * 2);
  put_le16(header + 32, static_cast<std::uint16_t>(channels * 2));
  put_le16(header + 34, 16);
  std::memcpy(header + 36, "data", 4);
  put_le32(header + 40, data_bytes);
  bool ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
            std::fwrite(samples.data(), 1, data_bytes, f) == data_bytes;
  return std::fclose(f) == 0 && ok;
}

}  // namespace k230::inspector
