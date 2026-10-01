#pragma once

#include <cstddef>
#include <cstdint>

namespace k230 {

inline std::uint16_t read16be(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | p[1]);
}

inline std::uint32_t read32be(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
         (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

inline std::uint64_t read64be(const std::uint8_t* p) {
  return (std::uint64_t{read32be(p)} << 32) | read32be(p + 4);
}

inline void write32be(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}

inline void write64be(std::uint8_t* p, std::uint64_t v) {
  write32be(p, static_cast<std::uint32_t>(v >> 32));
  write32be(p + 4, static_cast<std::uint32_t>(v));
}

}  // namespace k230
