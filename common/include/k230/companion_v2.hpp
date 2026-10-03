#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace k230::companion {
using Json = nlohmann::json;
constexpr std::size_t kMaxLine = 16384;
constexpr std::int64_t kTtlUs = 750000;
constexpr const char* kPolicyVersion = "age-10-15-v1";
struct Rect {
  int x = 0, y = 0, width = 0, height = 0;
  bool operator==(const Rect& r) const {
    return x == r.x && y == r.y && width == r.width && height == r.height;
  }
};
struct Probabilities {
  double porn = 0, hentai = 0, sexy = 0;
  bool valid() const;
  double explicit_score() const;
  bool hentai_dominant() const { return hentai > porn && porn < .60; }
};
struct AgeProfile {
  int age = 0;
  std::int64_t revision = 0;
  double cover() const { return age <= 12 ? .60 : .70; }
  double shield() const { return age <= 12 ? .80 : .85; }
  double exit() const { return age <= 12 ? .90 : .95; }
  int repetition_limit() const { return age <= 12 ? 2 : 3; }
  bool valid() const { return age >= 10 && age <= 15 && revision >= 1; }
  std::string name() const { return age <= 12 ? "10-12" : "13-15"; }
};
std::string uuid();
bool valid_uuid(const std::string& s);
std::int64_t decimal(const Json& value, bool positive = false);
Json profile_json(const AgeProfile& profile);
AgeProfile profile_from_json(const Json& value);
Json rect_json(const Rect& rect);
Rect rect_from_json(const Json& value);
Probabilities scores_from_json(const Json& value);
Json scores_json(const Probabilities& scores, bool explicit_field = true);

// Structural and semantic validation throws only fixed-code runtime errors.
Json parse(const std::string& payload);
void validate(const Json& message);
std::string line(const Json& message);
std::vector<std::uint8_t> control_record(const Json& message);
std::optional<Json> decode_control(const std::vector<std::uint8_t>& bytes);
std::vector<Rect> validate_decision(const Json& decision, const Json& state,
                                  std::int64_t phone_now_us, bool clock_verified);
Rect map_rect(Rect rect, int width, int height, int rotation, Rect viewport);

// Feed bytes with a single monotonic clock; idle is allowed, assembly is <=500ms.
class LineAssembler {
 public:
  std::vector<Json> feed(const std::uint8_t* data, std::size_t size, std::int64_t now_us);
  void check_deadline(std::int64_t now_us) const;
 private:
  std::string bytes_;
  std::int64_t started_us_ = -1;
};
}  // namespace k230::companion
