#include "k230/verdict.hpp"

#include <cstdio>

namespace k230 {

const char* to_string(Action a) {
  switch (a) {
    case Action::None: return "none";
    case Action::Log: return "log";
    case Action::Warn: return "warn";
    case Action::Block: return "block";
    case Action::Delete: return "delete";
  }
  return "none";
}

const char* to_string(Category c) {
  switch (c) {
    case Category::Safe: return "safe";
    case Category::Nudity: return "nudity";
    case Category::Violence: return "violence";
    case Category::Profanity: return "profanity";
    case Category::Unknown: break;
  }
  return "unknown";
}

std::string to_json_line(const Verdict& v) {
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "{\"seq\":%u,\"pts_us\":%lld,\"action\":\"%s\",\"category\":\"%s\",\"confidence\":%.3f}\n",
                v.sequence, static_cast<long long>(v.pts_us), to_string(v.action),
                to_string(v.category), static_cast<double>(v.confidence));
  return buf;
}

}  // namespace k230
