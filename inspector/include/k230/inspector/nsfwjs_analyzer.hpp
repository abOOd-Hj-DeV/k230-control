#pragma once

#include <memory>
#include <string>
#include <vector>

#include "k230/inspector/analyzer.hpp"

namespace k230::inspector {

struct NsfwjsConfig {
  std::string model_path = "models/nsfwjs-mobilenet-v2.onnx";
  int threads = 1;
};

std::vector<float> nsfwjs_input(const VideoFrame& frame);
std::unique_ptr<Analyzer> make_nsfwjs_analyzer(const NsfwjsConfig& config);

}  // namespace k230::inspector
