#include "k230/inspector/frame_dump.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
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

void put_be32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0xffffffffu) {
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
  }
  return crc;
}

bool write_chunk(std::FILE* file, const char* type, const std::uint8_t* data, std::size_t size) {
  std::uint8_t length[4], checksum[4];
  put_be32(length, static_cast<std::uint32_t>(size));
  std::uint32_t crc = crc32(reinterpret_cast<const std::uint8_t*>(type), 4);
  crc = crc32(data, size, crc);
  put_be32(checksum, ~crc);
  return std::fwrite(length, 1, 4, file) == 4 && std::fwrite(type, 1, 4, file) == 4 &&
         (size == 0 || std::fwrite(data, 1, size, file) == size) &&
         std::fwrite(checksum, 1, 4, file) == 4;
}

}  // namespace

bool write_png(const VideoFrame& frame, const std::string& path) {
  if (frame.width == 0 || frame.height == 0 || frame.width > 4096 || frame.height > 4096) return false;
  const auto chroma = static_cast<std::size_t>((frame.width + 1) / 2) * ((frame.height + 1) / 2);
  if (frame.data.size() < frame.luma_size() + chroma * 2) return false;
  const std::size_t row_size = static_cast<std::size_t>(frame.width) * 3 + 1;
  const std::size_t raw_size = row_size * frame.height;
  if (raw_size > std::numeric_limits<std::uint32_t>::max() - (raw_size / 65535 + 1) * 5 - 6) return false;

  std::vector<std::uint8_t> raw(raw_size);
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    auto* row = raw.data() + y * row_size;
    row[0] = 0;
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      const int Y = frame.y()[y * frame.width + x];
      const int Cb = frame.cb(x / 2, y / 2) - 128;
      const int Cr = frame.cr(x / 2, y / 2) - 128;
      const int c = 298 * (Y - 16);
      row[1 + x * 3] = clamp8((c + 409 * Cr + 128) >> 8);
      row[2 + x * 3] = clamp8((c - 100 * Cb - 208 * Cr + 128) >> 8);
      row[3 + x * 3] = clamp8((c + 516 * Cb + 128) >> 8);
    }
  }

  std::vector<std::uint8_t> zlib;
  zlib.reserve(raw_size + (raw_size / 65535 + 1) * 5 + 6);
  zlib.push_back(0x78);
  zlib.push_back(0x01);
  std::uint32_t a = 1, b = 0;
  for (std::size_t pos = 0; pos < raw.size();) {
    const std::size_t count = std::min<std::size_t>(65535, raw.size() - pos);
    const auto n = static_cast<std::uint16_t>(count);
    zlib.push_back(pos + count == raw.size() ? 1 : 0);
    zlib.push_back(static_cast<std::uint8_t>(n));
    zlib.push_back(static_cast<std::uint8_t>(n >> 8));
    zlib.push_back(static_cast<std::uint8_t>(~n));
    zlib.push_back(static_cast<std::uint8_t>(~n >> 8));
    for (std::size_t i = 0; i < count; ++i) {
      a = (a + raw[pos + i]) % 65521;
      b = (b + a) % 65521;
    }
    zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                raw.begin() + static_cast<std::ptrdiff_t>(pos + count));
    pos += count;
  }
  std::uint8_t adler[4];
  put_be32(adler, (b << 16) | a);
  zlib.insert(zlib.end(), adler, adler + 4);

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const std::uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  std::uint8_t ihdr[13] = {};
  put_be32(ihdr, frame.width);
  put_be32(ihdr + 4, frame.height);
  ihdr[8] = 8;
  ihdr[9] = 2;
  const bool ok = std::fwrite(signature, 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, 13) &&
                  write_chunk(f, "IDAT", zlib.data(), zlib.size()) && write_chunk(f, "IEND", nullptr, 0);
  return std::fclose(f) == 0 && ok;
}

bool write_wav(const std::vector<std::int16_t>& samples, std::uint32_t sample_rate, std::uint16_t channels,
               const std::string& path) {
  if (samples.size() > (std::numeric_limits<std::uint32_t>::max() - 36) / 2) return false;
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
  std::vector<std::uint8_t> pcm(data_bytes);
  for (std::size_t i = 0; i < samples.size(); ++i) put_le16(pcm.data() + 2 * i, static_cast<std::uint16_t>(samples[i]));
  bool ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
            std::fwrite(pcm.data(), 1, pcm.size(), f) == pcm.size();
  return std::fclose(f) == 0 && ok;
}

TestCapture::TestCapture(std::string directory) : directory_(std::move(directory)) {}

void TestCapture::push_audio(const PcmChunk& chunk) {
  if (start_pts_us_ < 0 || chunk.pts_us < start_pts_us_ + kDurationUs) audio_.push(chunk);
}

void TestCapture::push_frame(const VideoFrame& frame) {
  if (frame.pts_us < 0) return;
  if (start_pts_us_ < 0) start_pts_us_ = frame.pts_us;
  if (frame.pts_us >= start_pts_us_ + kDurationUs) return;
  const auto second = static_cast<std::uint32_t>((frame.pts_us - start_pts_us_) / 1'000'000);
  if (second < next_second_) return;
  next_second_ = second + 1;
  const std::string name = directory_ + "/second_" + std::to_string(second) + "_pts_" +
                           std::to_string(frame.pts_us) + ".png";
  if (!write_png(frame, name)) failed_ = true;
  else ++images_written_;
}

bool TestCapture::finish() {
  if (start_pts_us_ < 0) return false;
  auto samples = audio_.extract(start_pts_us_, start_pts_us_ + kDurationUs, &audio_filled_us_);
  const std::string name = directory_ + "/audio_pts_" + std::to_string(start_pts_us_) + "_8s.wav";
  return write_wav(samples, audio_.sample_rate(), audio_.channels(), name) && !failed_ &&
         images_written_ == 8 && audio_filled_us_ == kDurationUs;
}

}  // namespace k230::inspector
