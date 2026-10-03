#include "k230/inspector/analyzer.hpp"

#include <cmath>
#include <sstream>

namespace k230::inspector {

std::string describe_scores(const Scores& scores) {
  std::ostringstream out;
  out << "nudity=" << scores.nudity << " rms=" << scores.audio_level;
  if (scores.nsfwjs) {
    const auto& nsfw = *scores.nsfwjs;
    out << " Drawing=" << nsfw.drawing << " Hentai=" << nsfw.hentai << " Neutral=" << nsfw.neutral
        << " Porn=" << nsfw.porn << " Sexy=" << nsfw.sexy
        << " region=" << scores.analysis_region + 1 << "/" << scores.analysis_regions
        << " analysis_ms=" << scores.analysis_ms
        << " layout_session=" << scores.layout_session << " layout_seq=" << scores.layout_sequence
        << " region_id=" << scores.source_region << " complete=" << scores.analysis_complete
        << " frame_ms=" << scores.frame_ms << " transform_ms=" << scores.transform_ms
        << " prepare_ms=" << scores.preparation_ms << " inference_ms=" << scores.inference_ms;
  }
  return out.str();
}

float HeuristicAnalyzer::skin_ratio(const VideoFrame& frame) {
  if (frame.width < 2 || frame.height < 2 || frame.data.size() < frame.luma_size() * 3 / 2) return 0.0f;
  const std::uint32_t cw = (frame.width + 1) / 2;
  const std::uint32_t ch = (frame.height + 1) / 2;
  // Sub-sample the chroma grid: 4 px steps keep this well under 1 ms at 800px.
  std::uint32_t total = 0, skin = 0;
  for (std::uint32_t cy = 0; cy < ch; cy += 2) {
    for (std::uint32_t cx = 0; cx < cw; cx += 2) {
      const std::uint8_t cb = frame.cb(cx, cy);
      const std::uint8_t cr = frame.cr(cx, cy);
      const std::uint8_t y = frame.y()[(cy * 2) * frame.width + cx * 2];
      ++total;
      // Classic YCbCr skin box (Chai & Ngan), with a luma floor to ignore
      // dark UI backgrounds that happen to have skin-like chroma.
      if (y > 60 && cb >= 77 && cb <= 127 && cr >= 133 && cr <= 173) ++skin;
    }
  }
  return total ? static_cast<float>(skin) / static_cast<float>(total) : 0.0f;
}

float HeuristicAnalyzer::rms(const std::vector<std::int16_t>& samples) {
  if (samples.empty()) return 0.0f;
  double acc = 0.0;
  for (auto s : samples) acc += static_cast<double>(s) * s;
  return static_cast<float>(std::sqrt(acc / static_cast<double>(samples.size())) / 32768.0);
}

Scores HeuristicAnalyzer::analyze(const SyncedSample& sample) {
  Scores s;
  s.nudity = skin_ratio(sample.frame);
  s.audio_level = rms(sample.audio);
  return s;
}

}  // namespace k230::inspector
