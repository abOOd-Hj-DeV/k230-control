#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace k230 {

constexpr std::uint16_t kLayoutPartial = 1;
constexpr std::uint16_t kLayoutInvalidate = 2;
constexpr std::size_t kLayoutHeaderSize = 76;
constexpr std::size_t kLayoutNodeSize = 24;
constexpr std::size_t kLayoutMaxNodes = 256;
constexpr std::size_t kLayoutMaxPayload = kLayoutHeaderSize + 256 + kLayoutMaxNodes * kLayoutNodeSize;

struct LayoutNode {
  std::uint32_t left = 0, top = 0, right = 0, bottom = 0;
  std::uint32_t kind = 0, id = 0;
};

struct LayoutSnapshot {
  std::uint16_t flags = 0;
  std::uint64_t session = 0, sequence = 0;
  std::int64_t sampled_at_us = 0, completed_at_us = 0, valid_from_us = 0;
  std::uint32_t width = 0, height = 0, rotation = 0;
  std::int32_t display_id = 0, window_id = -1;
  std::string package;
  std::vector<LayoutNode> nodes;
};

std::uint32_t layout_payload_size(const std::uint8_t prefix[4]);
std::optional<LayoutSnapshot> decode_layout(const std::uint8_t* payload, std::size_t size);
std::string describe_layout(const LayoutSnapshot& snapshot);

class LayoutCache {
 public:
  bool push(LayoutSnapshot snapshot);
  void clear();
  std::optional<LayoutSnapshot> match(std::int64_t pts_us, std::int64_t max_age_us = 250'000) const;

 private:
  mutable std::mutex mutex_;
  std::deque<LayoutSnapshot> snapshots_;
  std::uint64_t session_ = 0, sequence_ = 0;
};

}  // namespace k230
