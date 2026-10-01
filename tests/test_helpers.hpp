#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "k230/byteorder.hpp"
#include "k230/inspector/frame.hpp"
#include "k230/media_packet.hpp"

namespace k230::test {

// Builds one scrcpy 12-byte-header packet exactly as the server writes it.
inline void append_scrcpy_packet(std::vector<std::uint8_t>& out, std::uint64_t pts_flags,
                                 const std::vector<std::uint8_t>& payload) {
  std::uint8_t h[12];
  write64be(h, pts_flags);
  write32be(h + 8, static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), h, h + 12);
  out.insert(out.end(), payload.begin(), payload.end());
}

inline void append_codec_id(std::vector<std::uint8_t>& out, const char id[4]) {
  out.insert(out.end(), id, id + 4);
}

inline void append_video_session(std::vector<std::uint8_t>& out, std::uint32_t w, std::uint32_t h, bool resized) {
  std::uint8_t p[12] = {};
  p[0] = 0x80;
  p[3] = resized ? 0x01 : 0x00;
  write32be(p + 4, w);
  write32be(p + 8, h);
  out.insert(out.end(), p, p + 12);
}

inline inspector::VideoFrame make_frame(std::int64_t pts_us, std::uint32_t w = 16, std::uint32_t h = 16,
                                        std::uint8_t y = 128, std::uint8_t cb = 128, std::uint8_t cr = 128) {
  inspector::VideoFrame f;
  f.width = w;
  f.height = h;
  f.pts_us = pts_us;
  f.format = inspector::PixelFormat::I420;
  f.data.assign(static_cast<std::size_t>(w) * h * 3 / 2, y);
  const std::size_t luma = static_cast<std::size_t>(w) * h;
  const std::size_t chroma = luma / 4;
  std::fill(f.data.begin() + luma, f.data.begin() + luma + chroma, cb);
  std::fill(f.data.begin() + luma + chroma, f.data.end(), cr);
  return f;
}

inline inspector::PcmChunk make_pcm(std::int64_t pts_us, std::int64_t duration_us, std::int16_t value,
                                    std::uint32_t rate = 48000, std::uint16_t channels = 2) {
  inspector::PcmChunk c;
  c.pts_us = pts_us;
  c.sample_rate = rate;
  c.channels = channels;
  const std::size_t frames = static_cast<std::size_t>(duration_us * rate / 1'000'000);
  c.samples.assign(frames * channels, value);
  return c;
}

}  // namespace k230::test
