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

struct NsfwjsRegion {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

std::vector<NsfwjsRegion> nsfwjs_regions(const VideoFrame& frame, std::uint32_t max_regions);
std::vector<float> nsfwjs_input(const VideoFrame& frame);
std::vector<float> nsfwjs_input(const VideoFrame& frame, const NsfwjsRegion& region);
std::unique_ptr<Analyzer> make_nsfwjs_analyzer(const NsfwjsConfig& config);

}  // namespace k230::inspector
