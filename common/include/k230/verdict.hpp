#pragma once

#include <cstdint>
#include <string>

namespace k230 {

// Decision produced by the big core for one synchronised (video, audio) window.
// Sent back to the little core over IPCMSG and forwarded to the companion app.
enum class Action : std::uint8_t {
  None = 0,       // nothing to do, not even logged
  Log = 1,        // safe, record for statistics
  Warn = 2,       // notify parent, no action on the phone
  Block = 3,      // companion app should close / cover the offending app
  Delete = 4,     // companion app should delete the offending content
};

enum class Category : std::uint8_t {
  Safe = 0,
  Nudity = 1,
  Violence = 2,
  Profanity = 3,  // audio
  Unknown = 255,
};

struct Verdict {
  std::int64_t pts_us = -1;         // PTS of the video frame this verdict refers to
  Action action = Action::None;
  Category category = Category::Safe;
  float confidence = 0.0f;          // 0..1, confidence in `category`
  std::uint32_t sequence = 0;       // monotonically increasing, for the companion app
};

const char* to_string(Action a);
const char* to_string(Category c);

// Single-line JSON, newline terminated, ready to be written to the companion
// app socket. Kept trivial on purpose: no JSON library dependency.
std::string to_json_line(const Verdict& v);

}  // namespace k230
