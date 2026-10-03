#pragma once

#include <memory>
#include <string>
#include <vector>

#include "k230/inspector/frame.hpp"

namespace k230::inspector {

constexpr std::uint32_t kUiSize = 640;
constexpr std::size_t kUiClasses = 21, kUiBoxes = 8400;

struct UiDetectorConfig {
  std::string model_path = "models/android-ui-yolov8n.onnx";
  int threads = 1;
  float confidence = 0.25f;
  float iou = 0.7f;
  std::uint32_t min_side = 64;
  float min_area = 0.01f;
  std::uint32_t max_regions = 8;
};

struct UiDetections {
  std::vector<ImageRegion> regions;
  std::uint32_t ignored_small = 0;
};

class RegionDetector {
 public:
  virtual ~RegionDetector() = default;
  virtual bool open() = 0;
  virtual UiDetections detect(const VideoFrame& frame) = 0;
};

std::vector<float> ui_input(const VideoFrame& frame);
UiDetections ui_regions(const VideoFrame& frame, const std::vector<float>& predictions,
                        const UiDetectorConfig& config);
std::unique_ptr<RegionDetector> make_ui_detector(const UiDetectorConfig& config);

}  // namespace k230::inspector
