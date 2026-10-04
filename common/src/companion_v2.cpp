#include "k230/companion_v2.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace k230::companion {
CaptureClockVerifier::Status CaptureClockVerifier::observe(std::int64_t pts, std::int64_t now) {
  if (status_ != Status::Pending) return status_;
  if (started_ < 0 || pts < 0 || now < started_ || now-started_ > 3000000 || ++samples_ > 8 ||
      (pts > now && pts-now > 50000) || (now >= pts && now-pts > kTtlUs) ||
      (last_pts_ >= 0 && (pts <= last_pts_ || now <= last_receive_))) return status_ = Status::Rejected;
  if (first_pts_ < 0) { first_pts_ = pts; first_receive_ = now; }
  last_pts_ = pts; last_receive_ = now;
  auto source_span = pts-first_pts_, receive_span = now-first_receive_;
  if (samples_ >= 3 && source_span >= 1000000 && receive_span >= 1000000) {
    if (std::abs(source_span-receive_span) > 100000) return status_ = Status::Rejected;
    status_ = Status::Accepted;
  }
  return status_;
}
namespace {
[[noreturn]] void fail(const char* code) { throw std::runtime_error(code); }
void require(bool condition, const char* code = "bounds") { if (!condition) fail(code); }
void keys(const Json& j, std::initializer_list<const char*> names) {
  require(j.is_object() && j.size() == names.size(), "malformed_json");
  for (auto name : names) require(j.contains(name), "malformed_json");
}
std::string str(const Json& j, std::size_t max = 255) {
  require(j.is_string(), "malformed_json");
  const auto s = j.get<std::string>();
  require(s.size() <= max, "bounds");
  return s;
}
void oneof(const Json& j, std::initializer_list<const char*> values) {
  auto s = str(j);
  for (auto value : values) if (s == value) return;
  fail("malformed_json");
}
int integer(const Json& j, int min, int max) {
  require(j.is_number_integer() && !j.is_boolean());
  require(!j.is_number_unsigned() || j.get<std::uint64_t>() <= static_cast<std::uint64_t>(INT64_MAX));
  auto n = j.get<std::int64_t>();
  require(n >= min && n <= max);
  return static_cast<int>(n);
}
void id(const Json& j) { require(valid_uuid(str(j, 36)), "malformed_json"); }
void nullable_id(const Json& j) { if (!j.is_null()) id(j); }
void package(const Json& j, bool empty = false) {
  auto s = str(j);
  require((empty && s.empty()) || std::regex_match(s, std::regex("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*")));
}
void boolean(const Json& j) { require(j.is_boolean(), "malformed_json"); }
void array(const Json& j, std::size_t min, std::size_t max) {
  require(j.is_array() && j.size() >= min && j.size() <= max);
}
void rotation(const Json& j) {
  int n = integer(j, 0, 270); require(n % 90 == 0, "invalid_transform");
}
void error(const Json& j) {
  if (j.is_null()) return;
  oneof(j, {"unsupported_version", "malformed_json", "bounds", "invalid_age", "policy_mismatch",
    "session_mismatch", "stream_mismatch", "stale", "future_pts", "clock_unverified", "wrong_screen",
    "invalid_transform", "invalid_scores", "invalid_evidence", "hentai_stage3_forbidden",
    "capability_missing", "permission_missing", "locked", "action_failed", "home_unverified",
    "event_conflict", "rate_limited", "busy", "storage_failed"});
}
void screen(const Json& j) {
  keys(j, {"screen_token", "content_epoch", "display_id", "width", "height", "rotation_deg", "window_id",
    "package", "sampled_at_us", "valid_from_us", "status"});
  id(j.at("screen_token")); decimal(j.at("content_epoch"), true);
  integer(j.at("display_id"), 0, 0); integer(j.at("width"), 1, 16384); integer(j.at("height"), 1, 16384);
  rotation(j.at("rotation_deg")); oneof(j.at("status"), {"verified", "invalid", "locked", "unsupported"});
  bool verified = j.at("status") == "verified";
  integer(j.at("window_id"), verified ? 0 : -1, INT32_MAX); package(j.at("package"), !verified);
  require(decimal(j.at("valid_from_us")) <= decimal(j.at("sampled_at_us")));
}
void rectangles(const Json& j) { array(j, 0, 8); for (const auto& r : j) rect_from_json(r); }
void policy(const Json& j) { if (!j.is_null()) profile_from_json(j); }
void region(const Json& j) {
  keys(j, {"track_id", "kind", "crop_frame_px", "scores", "evidence"});
  decimal(j.at("track_id"), true); oneof(j.at("kind"), {"Image", "BackgroundImage", "Video"});
  rect_from_json(j.at("crop_frame_px")); scores_from_json(j.at("scores"));
  const auto& e = j.at("evidence");
  keys(e, {"route", "observations", "analysis_continuity_id", "analysis_complete"});
  oneof(e.at("route"), {"explicit", "hentai_dominant"}); id(e.at("analysis_continuity_id"));
  boolean(e.at("analysis_complete")); require(e.at("analysis_complete") == true, "invalid_evidence");
  array(e.at("observations"), 3, 32);
  for (const auto& o : e.at("observations")) {
    keys(o, {"pts_us", "porn", "hentai", "sexy"}); decimal(o.at("pts_us"));
    for (auto k : {"porn", "hentai", "sexy"}) require(o.at(k).is_number(), "invalid_scores");
    require(Probabilities{o.at("porn"), o.at("hentai"), o.at("sexy")}.valid(), "invalid_scores");
  }
}
std::uint8_t kind(const Json& j) {
  const std::array<const char*, 7> types{{"decision", "state", "ack", "bind", "bound", "released", "hello"}};
  for (std::size_t i = 0; i < types.size(); ++i) if (j.at("type") == types[i]) return i + 1;
  fail("unsupported_version");
}
bool inside(Rect r, int w, int h) {
  return r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 &&
    static_cast<std::int64_t>(r.x) + r.width <= w && static_cast<std::int64_t>(r.y) + r.height <= h;
}
}  // namespace

bool Probabilities::valid() const {
  return std::isfinite(porn) && std::isfinite(hentai) && std::isfinite(sexy) &&
    porn >= 0 && porn <= 1 && hentai >= 0 && hentai <= 1 && sexy >= 0 && sexy <= 1 &&
    porn + hentai + sexy <= 1.001;
}
double Probabilities::explicit_score() const { return std::clamp(porn + hentai, 0.0, 1.0); }
bool valid_uuid(const std::string& s) {
  return std::regex_match(s, std::regex("[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"));
}
std::string uuid() {
  std::array<unsigned char, 16> bytes{};
  std::ifstream random("/dev/urandom", std::ios::binary);
  require(static_cast<bool>(random.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "storage_failed");
  bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128;
  std::string out; const char* hex = "0123456789abcdef";
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
    out += hex[bytes[i] >> 4]; out += hex[bytes[i] & 15];
  }
  return out;
}
std::int64_t decimal(const Json& j, bool positive) {
  auto s = str(j, 19);
  require(std::regex_match(s, std::regex("0|[1-9][0-9]{0,18}")));
  require(s.size() < 19 || s <= "9223372036854775807");
  auto n = std::stoll(s); require(!positive || n >= 1); return n;
}
Json profile_json(const AgeProfile& p) {
  require(p.valid(), "invalid_age");
  return {{"policy_version", kPolicyVersion}, {"policy_revision", std::to_string(p.revision)},
    {"age", p.age}, {"profile", p.name()}};
}
AgeProfile profile_from_json(const Json& j) {
  keys(j, {"policy_version", "policy_revision", "age", "profile"});
  require(j.at("policy_version") == kPolicyVersion, "policy_mismatch");
  require(j.at("age").is_number_integer() && !j.at("age").is_boolean(), "invalid_age");
  auto n = j.at("age").get<double>(); require(n >= 10 && n <= 15, "invalid_age");
  AgeProfile p{static_cast<int>(n), decimal(j.at("policy_revision"), true)};
  require(j.at("profile") == p.name(), "policy_mismatch"); return p;
}
Json rect_json(const Rect& r) { return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}}; }
Rect rect_from_json(const Json& j) {
  keys(j, {"x", "y", "width", "height"});
  Rect r{integer(j.at("x"), 0, 16384), integer(j.at("y"), 0, 16384),
    integer(j.at("width"), 1, 16384), integer(j.at("height"), 1, 16384)};
  require(static_cast<std::int64_t>(r.x) + r.width <= 16384 && static_cast<std::int64_t>(r.y) + r.height <= 16384);
  return r;
}
Probabilities scores_from_json(const Json& j) {
  keys(j, {"porn", "hentai", "sexy", "explicit_score"});
  for (auto k : {"porn", "hentai", "sexy", "explicit_score"}) require(j.at(k).is_number(), "invalid_scores");
  Probabilities p{j.at("porn").get<double>(), j.at("hentai").get<double>(), j.at("sexy").get<double>()};
  require(p.valid(), "invalid_scores");
  double e = j.at("explicit_score").get<double>();
  require(std::isfinite(e) && e >= 0 && e <= 1 && std::abs(e - p.explicit_score()) <= 1e-6, "invalid_scores");
  return p;
}
Json scores_json(const Probabilities& p, bool explicit_field) {
  require(p.valid(), "invalid_scores");
  Json j{{"porn", p.porn}, {"hentai", p.hentai}, {"sexy", p.sexy}};
  if (explicit_field) j["explicit_score"] = p.explicit_score();
  return j;
}

Json parse(const std::string& payload) {
  require(!payload.empty() && payload.size() < kMaxLine);
  require(payload.front() != '\xef' && payload.find('\0') == std::string::npos &&
    payload.find('\r') == std::string::npos && payload.find('\n') == std::string::npos, "malformed_json");
  std::vector<std::set<std::string>> objects;
  std::size_t tokens = 0;
  auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
    require(depth <= 10 && ++tokens <= 4096);
    if (event == Json::parse_event_t::object_start) objects.emplace_back();
    if (event == Json::parse_event_t::key) {
      require(!objects.empty()); auto& keys_seen = objects.back();
      require(keys_seen.insert(value.get<std::string>()).second, "malformed_json");
      require(keys_seen.size() <= 32);
    }
    if (event == Json::parse_event_t::object_end) objects.pop_back();
    if (event == Json::parse_event_t::array_end) require(value.size() <= 32);
    return true;
  };
  Json j;
  try { j = Json::parse(payload, callback, true, false); }
  catch (const Json::exception&) { fail("malformed_json"); }
  validate(j); return j;
}

void validate(const Json& j) {
  require(j.is_object() && j.contains("v") && j.contains("type"), "malformed_json");
  require(j.at("v").is_number_integer() && j.at("v") == 2, "unsupported_version");
  auto type = str(j.at("type"));
  if (type == "hello") {
    keys(j, {"v", "type", "session_id", "phone_boot_id", "versions", "max_line_bytes", "phone_time_us", "clock",
      "capabilities", "policy", "screen"});
    id(j.at("session_id")); id(j.at("phone_boot_id")); array(j.at("versions"), 1, 1);
    integer(j.at("versions").at(0), 2, 2); integer(j.at("max_line_bytes"), 16384, 16384);
    decimal(j.at("phone_time_us")); require(j.at("clock") == "android_system_nano_time_us", "clock_unverified");
    array(j.at("capabilities"), 0, 3); std::set<std::string> seen;
    for (const auto& c : j.at("capabilities")) {
      oneof(c, {"cover_region", "calm_shield", "home"}); require(seen.insert(str(c)).second);
    }
    policy(j.at("policy")); if (!j.at("screen").is_null()) screen(j.at("screen")); return;
  }
  require(j.contains("session_id") && j.contains("seq") && j.contains("stream_id"), "malformed_json");
  id(j.at("session_id")); id(j.at("stream_id")); decimal(j.at("seq"), true);
  if (type == "bind") {
    keys(j, {"v", "type", "session_id", "seq", "stream_id", "pts_clock", "capture_pts_us", "capture"});
    if (!j.at("capture_pts_us").is_null()) decimal(j.at("capture_pts_us"));
    require(j.at("pts_clock") == "android_system_nano_time_us", "clock_unverified");
    const auto& c = j.at("capture"); keys(c, {"source", "display_id", "mirror", "custom_crop", "custom_rotation"});
    require(c.at("source") == "scrcpy-4.0-display" || c.at("source") == "android-mediaprojection-display", "invalid_transform"); integer(c.at("display_id"), 0, 0);
    for (auto k : {"mirror", "custom_crop", "custom_rotation"}) { boolean(c.at(k)); require(c.at(k) == false, "invalid_transform"); }
  } else if (type == "bound") {
    keys(j, {"v", "type", "session_id", "seq", "request_seq", "stream_id", "status", "error", "phone_time_us"});
    decimal(j.at("request_seq"), true); decimal(j.at("phone_time_us"));
    oneof(j.at("status"), {"pending", "accepted", "rejected"}); error(j.at("error"));
    require((j.at("status") != "rejected") == j.at("error").is_null());
  } else if (type == "state") {
    keys(j, {"v", "type", "session_id", "seq", "stream_id", "phone_time_us", "policy", "screen", "protection", "health"});
    decimal(j.at("phone_time_us")); policy(j.at("policy")); if (!j.at("screen").is_null()) screen(j.at("screen"));
    const auto& p = j.at("protection"); keys(p, {"stage", "event_id", "action_revision", "target_screen_token", "applied_at_us", "covered_rects", "release_pending"});
    int stage = integer(p.at("stage"), 0, 3); nullable_id(p.at("event_id"));
    require((stage == 0) == p.at("event_id").is_null() && (stage == 0) == p.at("applied_at_us").is_null());
    require((stage == 0) == p.at("action_revision").is_null() && (stage == 0) == p.at("target_screen_token").is_null());
    if (stage) { decimal(p.at("action_revision"),true); id(p.at("target_screen_token")); }
    if (stage) decimal(p.at("applied_at_us"));
    rectangles(p.at("covered_rects")); boolean(p.at("release_pending"));
    require(stage != 0 || p.at("covered_rects").empty());
    require(stage != 1 || !p.at("covered_rects").empty());
    if (!j.at("screen").is_null()) {
      const auto& s = j.at("screen");
      for (const auto& r : p.at("covered_rects")) require(inside(rect_from_json(r),s.at("width"),s.at("height")));
      if (stage == 2) require(p.at("covered_rects") == Json::array({rect_json({0,0,s.at("width"),s.at("height")})}));
    }
    const auto& h = j.at("health"); keys(h, {"accessibility", "keystore", "pairing", "outbox_count", "cloud"});
    boolean(h.at("accessibility")); oneof(h.at("keystore"), {"ready", "locked", "failed"});
    oneof(h.at("pairing"), {"paired", "unpaired", "revoked", "key_lost"}); integer(h.at("outbox_count"), 0, 10000);
    oneof(h.at("cloud"), {"online", "offline", "unconfigured", "auth_error"});
  } else if (type == "decision") {
    keys(j, {"v", "type", "session_id", "seq", "stream_id", "event_id", "action_revision", "pts_us", "expires_at_us",
      "screen_token", "content_epoch", "package", "window_id", "policy", "frame", "transform", "requested_stage",
      "requested_action", "reason", "regions"});
    id(j.at("event_id")); id(j.at("screen_token")); decimal(j.at("action_revision"), true);
    auto pts = decimal(j.at("pts_us")); auto expiry = decimal(j.at("expires_at_us"));
    require(pts <= INT64_MAX - kTtlUs && expiry == pts + kTtlUs, "stale");
    decimal(j.at("content_epoch"), true); package(j.at("package")); integer(j.at("window_id"), 0, INT32_MAX);
    profile_from_json(j.at("policy")); const auto& f = j.at("frame"); keys(f, {"width", "height", "display_rotation_deg"});
    integer(f.at("width"), 1, 16384); integer(f.at("height"), 1, 16384); rotation(f.at("display_rotation_deg"));
    const auto& t = j.at("transform"); keys(t, {"rotation_cw_deg", "viewport_display_px"});
    integer(t.at("rotation_cw_deg"), 0, 0); rect_from_json(t.at("viewport_display_px"));
    int stage = integer(j.at("requested_stage"), 1, 3);
    require(j.at("requested_action") == (stage == 1 ? "cover_region" : stage == 2 ? "calm_shield" : "home"));
    oneof(j.at("reason"), {"threshold", "repetition"}); require(j.at("reason") != "repetition" || stage == 2, "invalid_evidence");
    array(j.at("regions"), 1, stage == 1 ? 8 : 1);
    std::set<std::int64_t> tracks;
    for (const auto& r : j.at("regions")) { region(r); require(tracks.insert(decimal(r.at("track_id"), true)).second); }
  } else if (type == "ack") {
    keys(j, {"v", "type", "session_id", "seq", "stream_id", "request_seq", "event_id", "action_revision", "status",
      "requested_stage", "executed_stage", "executed_action", "executed_at_us", "screen_token", "display_rects", "error"});
    decimal(j.at("request_seq"), true); id(j.at("event_id")); id(j.at("screen_token")); decimal(j.at("action_revision"), true);
    oneof(j.at("status"), {"executed", "duplicate", "rejected", "failed", "pending"});
    int requested = integer(j.at("requested_stage"), 1, 3), actual = integer(j.at("executed_stage"), 0, 3);
    require(j.at("executed_action") == (actual == 0 ? "none" : actual == 1 ? "cover_region" : actual == 2 ? "calm_shield" : "home"));
    if (!j.at("executed_at_us").is_null()) decimal(j.at("executed_at_us"));
    require(actual == 0 || !j.at("executed_at_us").is_null()); rectangles(j.at("display_rects")); error(j.at("error"));
    require(actual != 0 || j.at("executed_at_us").is_null());
    require((actual != 1 && actual != 2) || !j.at("display_rects").empty());
    require(actual <= requested);
    require(actual == 1 || actual == 2 || j.at("display_rects").empty());
    if (j.at("status") == "executed") require(actual == requested && j.at("error").is_null());
    if (j.at("status") == "pending") require(requested == 3 && (actual == 0 || actual == 2) && j.at("error").is_null());
    if (j.at("status") == "failed" || j.at("status") == "rejected") require(!j.at("error").is_null());
  } else if (type == "released") {
    keys(j, {"v", "type", "session_id", "seq", "stream_id", "event_id", "action_revision", "reason", "released_at_us",
      "previous_screen_token", "new_screen_token"});
    id(j.at("event_id")); decimal(j.at("action_revision"), true); decimal(j.at("released_at_us"));
    id(j.at("previous_screen_token")); nullable_id(j.at("new_screen_token"));
    oneof(j.at("reason"), {"verified_navigation", "verified_new_content", "guardian_grant"});
    require(j.at("reason") == "guardian_grant" || (!j.at("new_screen_token").is_null() &&
      j.at("new_screen_token") != j.at("previous_screen_token")));
  } else fail("unsupported_version");
}
std::string line(const Json& j) { validate(j); auto s = j.dump(); require(s.size() < kMaxLine); return s + '\n'; }
std::vector<std::uint8_t> control_record(const Json& j) {
  auto s = line(j); s.pop_back(); auto n = static_cast<std::uint32_t>(s.size());
  std::vector<std::uint8_t> out{'J', 2, kind(j), 0, static_cast<std::uint8_t>(n >> 24),
    static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)};
  out.insert(out.end(), s.begin(), s.end()); return out;
}
std::optional<Json> decode_control(const std::vector<std::uint8_t>& b) {
  try {
    require(b.size() >= 9 && b.size() <= 16391 && b[0] == 'J' && b[1] == 2 && b[3] == 0);
    std::uint32_t n = static_cast<std::uint32_t>(b[4]) << 24 | static_cast<std::uint32_t>(b[5]) << 16 |
      static_cast<std::uint32_t>(b[6]) << 8 | b[7]; require(n == b.size() - 8);
    auto j = parse(std::string(b.begin() + 8, b.end())); require(b[2] == kind(j)); return j;
  } catch (const std::exception&) { return std::nullopt; }
}
Rect map_rect(Rect r, int w, int h, int rotation_deg, Rect viewport) {
  require(w >= 1 && h >= 1 && w <= 16384 && h <= 16384 && inside(r, w, h), "invalid_transform");
  require(inside(viewport, 16384, 16384) && rotation_deg >= 0 && rotation_deg <= 270 && rotation_deg % 90 == 0, "invalid_transform");
  int rw = rotation_deg % 180 ? h : w, rh = rotation_deg % 180 ? w : h;
  std::array<std::pair<int, int>, 4> corners{{{r.x, r.y}, {r.x + r.width, r.y}, {r.x, r.y + r.height}, {r.x + r.width, r.y + r.height}}};
  double minx = 1e9, miny = 1e9, maxx = -1, maxy = -1;
  for (auto [x, y] : corners) {
    int rx = x, ry = y;
    if (rotation_deg == 90) { rx = h - y; ry = x; }
    if (rotation_deg == 180) { rx = w - x; ry = h - y; }
    if (rotation_deg == 270) { rx = y; ry = w - x; }
    double dx = viewport.x + static_cast<double>(rx) * viewport.width / rw;
    double dy = viewport.y + static_cast<double>(ry) * viewport.height / rh;
    minx = std::min(minx, dx); miny = std::min(miny, dy); maxx = std::max(maxx, dx); maxy = std::max(maxy, dy);
  }
  Rect mapped{static_cast<int>(std::floor(minx)), static_cast<int>(std::floor(miny)),
    static_cast<int>(std::ceil(maxx) - std::floor(minx)), static_cast<int>(std::ceil(maxy) - std::floor(miny))};
  require(inside(mapped, viewport.x + viewport.width, viewport.y + viewport.height), "invalid_transform"); return mapped;
}
std::vector<Rect> validate_decision(const Json& d, const Json& state, std::int64_t now, bool clock_verified) {
  validate(d); validate(state); require(d.at("type") == "decision" && state.at("type") == "state");
  require(clock_verified, "clock_unverified"); require(now >= 0);
  require(d.at("session_id") == state.at("session_id"), "session_mismatch");
  require(d.at("stream_id") == state.at("stream_id"), "stream_mismatch");
  require(!state.at("screen").is_null(), "wrong_screen"); const auto& s = state.at("screen");
  require(s.at("status") != "locked", "locked"); require(s.at("status") == "verified", "wrong_screen");
  for (auto k : {"screen_token", "content_epoch", "package", "window_id"}) require(d.at(k) == s.at(k), "wrong_screen");
  require(d.at("policy") == state.at("policy"), "policy_mismatch");
  require(state.at("health").at("accessibility") == true, "permission_missing");
  require(state.at("health").at("keystore") == "ready", "locked");
  require(state.at("health").at("pairing") == "paired", "session_mismatch");
  auto pts = decimal(d.at("pts_us")), sampled = decimal(s.at("sampled_at_us"));
  require(pts <= now || pts - now <= 50000, "future_pts");
  require(now <= pts || now - pts <= kTtlUs, "stale"); require(now <= decimal(d.at("expires_at_us")), "stale");
  require(sampled <= now && now - sampled <= 500000, "stale");
  auto w = integer(d.at("frame").at("width"), 1, 16384), h = integer(d.at("frame").at("height"), 1, 16384);
  require(d.at("frame").at("display_rotation_deg") == s.at("rotation_deg"), "invalid_transform");
  Rect viewport = rect_from_json(d.at("transform").at("viewport_display_px"));
  int sw = s.at("width"), sh = s.at("height");
  require(viewport == Rect{0, 0, sw, sh}, "invalid_transform");
  double sx = static_cast<double>(sw) / w, sy = static_cast<double>(sh) / h;
  require(std::abs(sx - sy) / std::max(sx, sy) <= .015, "invalid_transform");
  AgeProfile p = profile_from_json(d.at("policy")); int stage = d.at("requested_stage");
  std::vector<Rect> mapped;
  for (const auto& r : d.at("regions")) {
    const auto& e = r.at("evidence"); bool hentai = e.at("route") == "hentai_dominant";
    require(!hentai || stage != 3, "hentai_stage3_forbidden");
    const auto& observations = e.at("observations");
    require(observations.size() >= (hentai || stage == 3 ? 5u : 3u), "invalid_evidence");
    std::int64_t first = -1, previous = -1;
    for (const auto& o : observations) {
      auto t = decimal(o.at("pts_us")); require(t >= decimal(s.at("valid_from_us")), "invalid_evidence");
      require(previous < 0 || (t > previous && t - previous <= 500000), "invalid_evidence");
      if (first < 0) first = t;
      previous = t;
      Probabilities score{o.at("porn"), o.at("hentai"), o.at("sexy")}; require(score.valid(), "invalid_scores");
      require(score.hentai_dominant() == hentai && (!hentai || score.hentai > .60), "invalid_evidence");
      double threshold = stage == 3 ? p.exit() : stage == 2 && d.at("reason") == "threshold" ? p.shield() : p.cover();
      require(score.explicit_score() >= threshold && (stage != 3 || score.porn >= .60), "invalid_evidence");
    }
    require(previous == pts && previous - first >= (hentai || stage == 3 ? 1000000 : 400000), "invalid_evidence");
    auto final = scores_from_json(r.at("scores")); const auto& last = observations.back();
    require(final.porn == last.at("porn").get<double>() && final.hentai == last.at("hentai").get<double>() &&
      final.sexy == last.at("sexy").get<double>(), "invalid_evidence");
    auto crop = rect_from_json(r.at("crop_frame_px")); require(inside(crop, w, h), "invalid_transform");
    const auto& protection = state.at("protection");
    if (!protection.at("applied_at_us").is_null()) {
      auto applied = decimal(protection.at("applied_at_us")); auto display_crop = map_rect(crop, w, h, 0, viewport);
      for (const auto& mask : protection.at("covered_rects")) {
        auto m = rect_from_json(mask);
        bool overlap = display_crop.x < m.x + m.width && m.x < display_crop.x + display_crop.width &&
          display_crop.y < m.y + m.height && m.y < display_crop.y + display_crop.height;
        if (overlap) for (const auto& o : observations) require(decimal(o.at("pts_us")) < applied, "invalid_evidence");
      }
      if (protection.at("stage").get<int>() >= 2)
        for (const auto& o : observations) require(decimal(o.at("pts_us")) < applied, "invalid_evidence");
    }
    mapped.push_back(map_rect(crop, w, h, 0, viewport));
  }
  return mapped;
}
void LineAssembler::check_deadline(std::int64_t now) const {
  require(started_us_ < 0 || (now >= started_us_ && now - started_us_ <= 500000), "stale");
}
std::vector<Json> LineAssembler::feed(const std::uint8_t* data, std::size_t size, std::int64_t now) {
  check_deadline(now); std::vector<Json> result;
  for (std::size_t i = 0; i < size; ++i) {
    if (started_us_ < 0) started_us_ = now;
    require(data[i] != 0 && data[i] != '\r', "malformed_json");
    if (data[i] == '\n') { result.push_back(parse(bytes_)); bytes_.clear(); started_us_ = -1; }
    else { require(bytes_.size() < kMaxLine - 1); bytes_.push_back(static_cast<char>(data[i])); }
  }
  return result;
}
}  // namespace k230::companion
