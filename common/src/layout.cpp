#include "k230/layout.hpp"

#include <limits>
#include <sstream>
#include <unordered_set>

namespace k230 {
namespace {
std::uint16_t u16(const std::uint8_t* p) { return p[0] | (static_cast<std::uint16_t>(p[1]) << 8); }
std::uint32_t u32(const std::uint8_t* p) {
  return u16(p) | (static_cast<std::uint32_t>(u16(p + 2)) << 16);
}
std::uint64_t u64(const std::uint8_t* p) {
  return u32(p) | (static_cast<std::uint64_t>(u32(p + 4)) << 32);
}
}

std::uint32_t layout_payload_size(const std::uint8_t prefix[4]) { return u32(prefix); }

std::optional<LayoutSnapshot> decode_layout(const std::uint8_t* p, std::size_t size) {
  if (size < kLayoutHeaderSize || size > kLayoutMaxPayload || u32(p) != 0x59414c4b || u16(p + 4) != 1) {
    return std::nullopt;
  }
  LayoutSnapshot s;
  s.flags = u16(p + 6);
  s.session = u64(p + 8);
  s.sequence = u64(p + 16);
  const auto sampled = u64(p + 24), completed = u64(p + 32), valid_from = u64(p + 40);
  if (s.flags > 3 || s.session == 0 || s.session > static_cast<std::uint64_t>(INT64_MAX) ||
      s.sequence == 0 || s.sequence > static_cast<std::uint64_t>(INT64_MAX) ||
      sampled > static_cast<std::uint64_t>(INT64_MAX) || completed > static_cast<std::uint64_t>(INT64_MAX) ||
      completed < sampled || valid_from > sampled) return std::nullopt;
  s.sampled_at_us = static_cast<std::int64_t>(sampled);
  s.completed_at_us = static_cast<std::int64_t>(completed);
  s.valid_from_us = static_cast<std::int64_t>(valid_from);
  s.width = u32(p + 48);
  s.height = u32(p + 52);
  s.rotation = u32(p + 56);
  s.display_id = static_cast<std::int32_t>(u32(p + 60));
  s.window_id = static_cast<std::int32_t>(u32(p + 64));
  const auto count = u32(p + 68), name_size = u32(p + 72);
  if (s.width == 0 || s.height == 0 || s.width > 16384 || s.height > 16384 ||
      s.rotation > 3 || s.display_id != 0 || count > kLayoutMaxNodes || name_size > 256 ||
      size != kLayoutHeaderSize + name_size + count * kLayoutNodeSize ||
      ((s.flags & kLayoutInvalidate) && count != 0)) return std::nullopt;
  s.package.assign(reinterpret_cast<const char*>(p + kLayoutHeaderSize), name_size);
  for (const unsigned char c : s.package) {
    if (c < 33 || c > 126) return std::nullopt;
  }
  std::unordered_set<std::uint32_t> ids;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto* n = p + kLayoutHeaderSize + name_size + i * kLayoutNodeSize;
    LayoutNode node{u32(n), u32(n + 4), u32(n + 8), u32(n + 12), u32(n + 16), u32(n + 20)};
    if (node.left >= node.right || node.top >= node.bottom || node.right > s.width ||
        node.bottom > s.height || node.kind > 3 || node.id == 0 || node.id > INT32_MAX ||
        !ids.insert(node.id).second) return std::nullopt;
    s.nodes.push_back(node);
  }
  return s;
}

std::string describe_layout(const LayoutSnapshot& s) {
  std::ostringstream out;
  out << "{\"session\":" << s.session << ",\"sequence\":" << s.sequence << ",\"flags\":" << s.flags
      << ",\"sampled_at_us\":" << s.sampled_at_us << ",\"completed_at_us\":" << s.completed_at_us
      << ",\"valid_from_us\":" << s.valid_from_us << ",\"width\":" << s.width << ",\"height\":" << s.height
      << ",\"rotation\":" << s.rotation << ",\"display_id\":" << s.display_id << ",\"window_id\":" << s.window_id
      << ",\"package\":\"";
  for (char c : s.package) {
    if (c == '"' || c == '\\') out << '\\';
    out << c;
  }
  out << "\",\"nodes\":[";
  for (std::size_t i = 0; i < s.nodes.size(); ++i) {
    const auto& n = s.nodes[i];
    if (i) out << ',';
    out << "{\"id\":" << n.id << ",\"kind\":" << n.kind << ",\"bounds\":[" << n.left << ','
        << n.top << ',' << n.right << ',' << n.bottom << "]}";
  }
  return out.str() + "]}";
}

bool LayoutCache::push(LayoutSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot.session != session_) {
    if (session_ != 0) return false;
    session_ = snapshot.session;
    sequence_ = 0;
  }
  if (snapshot.sequence <= sequence_) return false;
  if (!snapshots_.empty() && (snapshot.sampled_at_us < snapshots_.back().sampled_at_us ||
                             snapshot.valid_from_us < snapshots_.back().valid_from_us)) return false;
  if (sequence_ && snapshot.sequence != sequence_ + 1) snapshots_.clear();
  sequence_ = snapshot.sequence;
  snapshots_.push_back(std::move(snapshot));
  while (snapshots_.size() > 64) snapshots_.pop_front();
  return true;
}

void LayoutCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshots_.clear();
  session_ = sequence_ = 0;
}

std::optional<LayoutSnapshot> LayoutCache::match(std::int64_t pts_us, std::int64_t max_age_us) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::int64_t barrier = -1;
  for (const auto& s : snapshots_) {
    if (s.valid_from_us <= pts_us && s.valid_from_us > barrier) barrier = s.valid_from_us;
  }
  for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
    if (it->valid_from_us > pts_us) continue;
    if (it->valid_from_us < barrier) return std::nullopt;
    if (it->flags & kLayoutInvalidate) return std::nullopt;
    if (it->completed_at_us > pts_us) continue;
    if (pts_us - it->sampled_at_us > max_age_us || it->completed_at_us - it->sampled_at_us > 50'000) {
      return std::nullopt;
    }
    return *it;
  }
  return std::nullopt;
}
}  // namespace k230
