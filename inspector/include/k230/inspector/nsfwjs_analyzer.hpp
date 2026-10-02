#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "k230/inspector/analyzer.hpp"

namespace k230::inspector {

struct NsfwjsConfig {
  std::string model_path = "models/nsfwjs-mobilenet-v2.onnx";
  int threads = 1;
  std::uint32_t max_regions = 9;
};

using NsfwjsRegion = ImageRegion;

class RegionAnalyzer {
 public:
  virtual ~RegionAnalyzer() = default;
  virtual bool open() = 0;
  virtual Scores analyze_region(const VideoFrame& frame, const ImageRegion& region) = 0;
};

std::unique_ptr<RegionAnalyzer> make_nsfwjs_region_analyzer(const NsfwjsConfig& config);

std::vector<NsfwjsRegion> nsfwjs_regions(const VideoFrame& frame, std::uint32_t max_regions);
std::vector<float> nsfwjs_input(const VideoFrame& frame);
std::vector<float> nsfwjs_input(const VideoFrame& frame, const NsfwjsRegion& region);
std::unique_ptr<Analyzer> make_nsfwjs_analyzer(const NsfwjsConfig& config);

}  // namespace k230::inspector
