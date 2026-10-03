#include "k230/inspector/ui_detector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "k230/log.hpp"

#ifdef K230_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace k230::inspector {
namespace {

struct Letterbox {
  double scale;
  std::uint32_t width, height, left, top;
};

Letterbox letterbox(const VideoFrame& frame) {
  if (!frame.width || !frame.height || frame.width > 4096 || frame.height > 4096) {
    throw std::invalid_argument("UI detection requires frame dimensions in [1,4096]");
  }
  const double scale = static_cast<double>(kUiSize) / std::max(frame.width, frame.height);
  const auto width = std::max(1u, static_cast<std::uint32_t>(std::nearbyint(frame.width * scale)));
  const auto height = std::max(1u, static_cast<std::uint32_t>(std::nearbyint(frame.height * scale)));
  return {scale, width, height, (kUiSize - width) / 2, (kUiSize - height) / 2};
}

void validate(const UiDetectorConfig& config) {
  if (config.threads < 1 || !std::isfinite(config.confidence) || config.confidence <= 0 ||
      config.confidence > 1 || !std::isfinite(config.iou) || config.iou <= 0 || config.iou > 1 ||
      !config.min_side || config.min_side > 4096 || !std::isfinite(config.min_area) ||
      config.min_area < 0 || config.min_area > 1 || !config.max_regions || config.max_regions > 256) {
    throw std::invalid_argument("invalid UI detector threads, confidence, IoU or region-size limits");
  }
}

std::array<float, 3> rgb(const VideoFrame& frame, std::uint32_t x, std::uint32_t y) {
  const int c = 298 * (frame.y()[static_cast<std::size_t>(y) * frame.width + x] - 16);
  const int cb = frame.cb(x / 2, y / 2) - 128;
  const int cr = frame.cr(x / 2, y / 2) - 128;
  return {static_cast<float>(std::clamp((c + 409 * cr + 128) >> 8, 0, 255)),
          static_cast<float>(std::clamp((c - 100 * cb - 208 * cr + 128) >> 8, 0, 255)),
          static_cast<float>(std::clamp((c + 516 * cb + 128) >> 8, 0, 255))};
}

struct Candidate {
  double left, top, right, bottom;
  float confidence;
};

double overlap(const Candidate& a, const Candidate& b) {
  const double intersection = std::max(0.0, std::min(a.right, b.right) - std::max(a.left, b.left)) *
                              std::max(0.0, std::min(a.bottom, b.bottom) - std::max(a.top, b.top));
  return intersection / ((a.right - a.left) * (a.bottom - a.top) +
                          (b.right - b.left) * (b.bottom - b.top) - intersection);
}

}  // namespace

std::vector<float> ui_input(const VideoFrame& frame) {
  const auto box = letterbox(frame);
  const auto chroma = static_cast<std::size_t>((frame.width + 1) / 2) * ((frame.height + 1) / 2);
  if (frame.data.size() < frame.luma_size() + 2 * chroma) {
    throw std::invalid_argument("UI detector received a truncated YUV frame");
  }
  constexpr std::size_t plane = kUiSize * kUiSize;
  std::vector<float> input(plane * 3, 114.0f / 255.0f);
  for (std::uint32_t y = 0; y < box.height; ++y) {
    const double sy = std::clamp((y + 0.5) * frame.height / box.height - 0.5, 0.0,
                                 static_cast<double>(frame.height - 1));
    const auto y0 = static_cast<std::uint32_t>(sy), y1 = std::min(y0 + 1, frame.height - 1);
    for (std::uint32_t x = 0; x < box.width; ++x) {
      const double sx = std::clamp((x + 0.5) * frame.width / box.width - 0.5, 0.0,
                                   static_cast<double>(frame.width - 1));
      const auto x0 = static_cast<std::uint32_t>(sx), x1 = std::min(x0 + 1, frame.width - 1);
      const auto a = rgb(frame, x0, y0), b = rgb(frame, x1, y0);
      const auto c = rgb(frame, x0, y1), d = rgb(frame, x1, y1);
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const double top = a[channel] + (b[channel] - a[channel]) * (sx - x0);
        const double bottom = c[channel] + (d[channel] - c[channel]) * (sx - x0);
        input[channel * plane + (y + box.top) * kUiSize + x + box.left] =
            static_cast<float>(std::round(top + (bottom - top) * (sy - y0)) / 255.0);
      }
    }
  }
  return input;
}

UiDetections ui_regions(const VideoFrame& frame, const std::vector<float>& predictions,
                        const UiDetectorConfig& config) {
  validate(config);
  const auto box = letterbox(frame);
  if (predictions.size() != (kUiClasses + 4) * kUiBoxes) {
    throw std::invalid_argument("UI detector output must be FP32 [1,25,8400]");
  }
  std::vector<Candidate> candidates;
  for (std::size_t i = 0; i < kUiBoxes; ++i) {
    float confidence = 0;
    std::size_t label = kUiClasses;
    for (std::size_t c = 0; c < kUiClasses; ++c) {
      const float score = predictions[(4 + c) * kUiBoxes + i];
      if (!std::isfinite(score) || score < 0 || score > 1) {
        throw std::runtime_error("UI detector produced invalid class probabilities");
      }
      if (score > confidence) { confidence = score; label = c; }
    }
    if (confidence < config.confidence || (label != 0 && label != 9)) continue;
    const double x = predictions[i], y = predictions[kUiBoxes + i];
    const double w = predictions[2 * kUiBoxes + i], h = predictions[3 * kUiBoxes + i];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0) {
      throw std::runtime_error("UI detector produced invalid box coordinates");
    }
    Candidate candidate{
        std::clamp((x - w / 2 - box.left) / box.scale, 0.0, static_cast<double>(frame.width)),
        std::clamp((y - h / 2 - box.top) / box.scale, 0.0, static_cast<double>(frame.height)),
        std::clamp((x + w / 2 - box.left) / box.scale, 0.0, static_cast<double>(frame.width)),
        std::clamp((y + h / 2 - box.top) / box.scale, 0.0, static_cast<double>(frame.height)), confidence};
    if (candidate.right > candidate.left && candidate.bottom > candidate.top) candidates.push_back(candidate);
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
    return a.confidence > b.confidence;
  });
  if (candidates.size() > 300) candidates.resize(300);
  UiDetections result;
  std::vector<Candidate> kept, ignored;
  for (const auto& candidate : candidates) {
    const double width = candidate.right - candidate.left, height = candidate.bottom - candidate.top;
    const bool small = width < config.min_side || height < config.min_side ||
                       width * height < config.min_area * static_cast<double>(frame.width) * frame.height;
    auto& group = small ? ignored : kept;
    if (std::any_of(group.begin(), group.end(), [&](const auto& previous) {
      return overlap(candidate, previous) > config.iou;
    })) continue;
    group.push_back(candidate);
    if (small) {
      ++result.ignored_small;
      continue;
    }
    const auto x = static_cast<std::uint32_t>(std::floor(candidate.left));
    const auto y = static_cast<std::uint32_t>(std::floor(candidate.top));
    const auto w = static_cast<std::uint32_t>(std::ceil(candidate.right)) - x;
    const auto h = static_cast<std::uint32_t>(std::ceil(candidate.bottom)) - y;
    if (result.regions.size() < config.max_regions) result.regions.push_back({x, y, w, h});
  }
  return result;
}

#ifdef K230_HAS_ONNX
namespace {
class UiDetector final : public RegionDetector {
 public:
  explicit UiDetector(UiDetectorConfig config) : config_(std::move(config)) {}
  bool open() override {
    if (session_) return true;
    try {
      validate(config_);
      Ort::SessionOptions options;
      options.SetIntraOpNumThreads(config_.threads);
      options.SetInterOpNumThreads(1);
      options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      session_ = std::make_unique<Ort::Session>(env_, config_.model_path.c_str(), options);
      if (session_->GetInputCount() != 1 || session_->GetOutputCount() != 1) {
        throw std::runtime_error("UI model must have one input and one output");
      }
      const auto input_type = session_->GetInputTypeInfo(0), output_type = session_->GetOutputTypeInfo(0);
      const auto input = input_type.GetTensorTypeAndShapeInfo(), output = output_type.GetTensorTypeAndShapeInfo();
      if (input.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          output.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          input.GetShape() != std::vector<std::int64_t>({1, 3, kUiSize, kUiSize}) ||
          output.GetShape() != std::vector<std::int64_t>({1, kUiClasses + 4, kUiBoxes})) {
        throw std::runtime_error("UI model requires FP32 NCHW [1,3,640,640] -> [1,25,8400]");
      }
      Ort::AllocatorWithDefaultOptions allocator;
      const auto metadata = session_->GetModelMetadata();
      const auto classes = metadata.LookupCustomMetadataMapAllocated("k230.ui.media_classes", allocator);
      const auto preprocessing = metadata.LookupCustomMetadataMapAllocated("k230.ui.input", allocator);
      if (!classes || std::string(classes.get()) != "0:BackgroundImage,9:Image" || !preprocessing ||
          std::string(preprocessing.get()) != "rgb_nchw_float32_0_1_letterbox_114_half_pixel") {
        throw std::runtime_error("incompatible UI model metadata; use tools/export_ui_detector.py");
      }
      input_name_ = session_->GetInputNameAllocated(0, allocator).get();
      output_name_ = session_->GetOutputNameAllocated(0, allocator).get();
      K230_LOG_INFO("ui") << "loaded " << config_.model_path << " min_side=" << config_.min_side
                          << " min_area=" << config_.min_area << " confidence=" << config_.confidence;
      return true;
    } catch (const std::exception& e) {
      session_.reset();
      K230_LOG_ERROR("ui") << e.what();
      return false;
    }
  }
  UiDetections detect(const VideoFrame& frame) override {
    if (!session_) throw std::runtime_error("UI detector is not open");
    auto pixels = ui_input(frame);
    const std::array<std::int64_t, 4> shape{1, 3, kUiSize, kUiSize};
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input = Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(), shape.data(), shape.size());
    const char* inputs[] = {input_name_.c_str()};
    const char* outputs[] = {output_name_.c_str()};
    auto predictions = session_->Run(Ort::RunOptions{nullptr}, inputs, &input, 1, outputs, 1);
    const auto& tensor = predictions.front();
    const auto count = tensor.GetTensorTypeAndShapeInfo().GetElementCount();
    const auto* data = tensor.GetTensorData<float>();
    return ui_regions(frame, std::vector<float>(data, data + count), config_);
  }
 private:
  UiDetectorConfig config_;
  Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "k230-ui"};
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_, output_name_;
};
}  // namespace
#endif

std::unique_ptr<RegionDetector> make_ui_detector(const UiDetectorConfig& config) {
#ifdef K230_HAS_ONNX
  return std::make_unique<UiDetector>(config);
#else
  (void)config;
  K230_LOG_ERROR("ui") << "UI detector requires a PC build with K230_ENABLE_ONNX=ON";
  return nullptr;
#endif
}
}  // namespace k230::inspector
