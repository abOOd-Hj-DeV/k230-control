#pragma once

#include <memory>
#include <optional>
#include <string>

#include "k230/inspector/sync_engine.hpp"

namespace k230::inspector {

struct NsfwjsScores {
  float drawing = 0.0f;
  float hentai = 0.0f;
  float neutral = 0.0f;
  float porn = 0.0f;
  float sexy = 0.0f;
};

// Per-sample scores in [0, 1]. Produced by the model, consumed by Policy.
struct Scores {
  float nudity = 0.0f;
  float violence = 0.0f;
  float profanity = 0.0f;  // from the audio window
  float audio_level = 0.0f;  // RMS of the window, diagnostic only
  std::optional<NsfwjsScores> nsfwjs;
  double analysis_ms = 0.0;
};

std::string describe_scores(const Scores& scores);

class Analyzer {
 public:
  virtual ~Analyzer() = default;
  virtual bool open() = 0;
  virtual Scores analyze(const SyncedSample& sample) = 0;
  virtual const char* name() const = 0;
};

// Development stand-in: skin-tone ratio in YCbCr for `nudity`, RMS for
// `audio_level`. Deterministic and dependency-free, so the whole pipeline can
// be exercised and unit-tested before a real model exists. NOT a classifier.
class HeuristicAnalyzer final : public Analyzer {
 public:
  bool open() override { return true; }
  Scores analyze(const SyncedSample& sample) override;
  const char* name() const override { return "heuristic"; }

  static float skin_ratio(const VideoFrame& frame);
  static float rms(const std::vector<std::int16_t>& samples);
};

// K230 KPU backend: runs a .kmodel produced by nncase through the nncase
// runtime (`nncase::runtime::interpreter`). Only available on the big core.
struct KpuConfig {
  std::string kmodel_path = "/sdcard/models/nsfw.kmodel";
  std::uint32_t input_width = 224;
  std::uint32_t input_height = 224;
};
std::unique_ptr<Analyzer> make_kpu_analyzer(const KpuConfig& config);

}  // namespace k230::inspector
