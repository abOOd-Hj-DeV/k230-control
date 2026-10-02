#include "k230/inspector/nsfwjs_analyzer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "k230/log.hpp"

#ifdef K230_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace k230::inspector {

namespace {

constexpr std::uint32_t kSize = 224;
constexpr std::uint32_t kMaximumRegions = 9;

std::array<float, 3> rgb(const VideoFrame& frame, std::uint32_t x, std::uint32_t y) {
  const int c = 298 * (frame.y()[static_cast<std::size_t>(y) * frame.width + x] - 16);
  const int cb = frame.cb(x / 2, y / 2) - 128;
  const int cr = frame.cr(x / 2, y / 2) - 128;
  return {
      std::clamp((c + 409 * cr + 128) >> 8, 0, 255) / 255.0f,
      std::clamp((c - 100 * cb - 208 * cr + 128) >> 8, 0, 255) / 255.0f,
      std::clamp((c + 516 * cb + 128) >> 8, 0, 255) / 255.0f,
  };
}

}  // namespace

std::vector<NsfwjsRegion> nsfwjs_regions(const VideoFrame& frame, std::uint32_t max_regions) {
  if (frame.width == 0 || frame.height == 0 || frame.width > 4096 || frame.height > 4096) {
    throw std::invalid_argument("NSFWJS requires a decoded frame between 1 and 4096 pixels per dimension");
  }
  if (max_regions < 1 || max_regions > kMaximumRegions) {
    throw std::invalid_argument("NSFWJS regions must be between 1 and 9");
  }
  std::vector<NsfwjsRegion> regions{{0, 0, frame.width, frame.height}};
  if (max_regions == 1) return regions;

  const bool portrait = frame.height > frame.width;
  const std::uint32_t short_side = std::min(frame.width, frame.height);
  const std::uint32_t long_side = std::max(frame.width, frame.height);
  if (short_side < kSize) return regions;
  if (static_cast<std::uint64_t>(long_side) * 4 <=
      static_cast<std::uint64_t>(short_side) * 5) {
    return regions;
  }

  const std::uint32_t window_long = std::max<std::uint32_t>(1, short_side * 4 / 5);
  const std::uint32_t travel = long_side - window_long;
  const std::uint32_t windows = max_regions - 1;
  for (std::uint32_t i = 0; i < windows; ++i) {
    const std::uint32_t offset =
        windows == 1 ? travel / 2 : static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(travel) * i + (windows - 1) / 2) / (windows - 1));
    const NsfwjsRegion region = portrait
        ? NsfwjsRegion{0, offset, short_side, window_long}
        : NsfwjsRegion{offset, 0, window_long, short_side};
    if (regions.back().x != region.x || regions.back().y != region.y ||
        regions.back().width != region.width || regions.back().height != region.height) {
      regions.push_back(region);
    }
  }
  return regions;
}

std::vector<float> nsfwjs_input(const VideoFrame& frame, const NsfwjsRegion& region) {
  if (frame.width == 0 || frame.height == 0 || frame.width > 4096 || frame.height > 4096) {
    throw std::invalid_argument("NSFWJS requires a decoded frame between 1 and 4096 pixels per dimension");
  }
  const std::size_t chroma_size = static_cast<std::size_t>((frame.width + 1) / 2) * ((frame.height + 1) / 2);
  if (frame.data.size() < frame.luma_size() + 2 * chroma_size) {
    throw std::invalid_argument("NSFWJS received a truncated YUV frame");
  }
  if (region.width == 0 || region.height == 0 || region.x >= frame.width || region.y >= frame.height ||
      region.width > frame.width - region.x || region.height > frame.height - region.y) {
    throw std::invalid_argument("NSFWJS received an invalid image region");
  }
  std::vector<float> input(kSize * kSize * 3);
  const float scale_x = static_cast<float>(region.width - 1) / (kSize - 1);
  const float scale_y = static_cast<float>(region.height - 1) / (kSize - 1);
  for (std::uint32_t y = 0; y < kSize; ++y) {
    const float sy = region.y + std::min(y * scale_y, static_cast<float>(region.height - 1));
    const auto y0 = static_cast<std::uint32_t>(sy);
    const auto y1 = std::min(y0 + 1, frame.height - 1);
    const float dy = sy - y0;
    for (std::uint32_t x = 0; x < kSize; ++x) {
      const float sx = region.x + std::min(x * scale_x, static_cast<float>(region.width - 1));
      const auto x0 = static_cast<std::uint32_t>(sx);
      const auto x1 = std::min(x0 + 1, frame.width - 1);
      const float dx = sx - x0;
      const auto a = rgb(frame, x0, y0);
      const auto b = rgb(frame, x1, y0);
      const auto c = rgb(frame, x0, y1);
      const auto d = rgb(frame, x1, y1);
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const float top = a[channel] + (b[channel] - a[channel]) * dx;
        const float bottom = c[channel] + (d[channel] - c[channel]) * dx;
        input[(y * kSize + x) * 3 + channel] = top + (bottom - top) * dy;
      }
    }
  }
  return input;
}

std::vector<float> nsfwjs_input(const VideoFrame& frame) {
  return nsfwjs_input(frame, NsfwjsRegion{0, 0, frame.width, frame.height});
}

#ifdef K230_HAS_ONNX
namespace {

class NsfwjsAnalyzer final : public Analyzer {
 public:
  explicit NsfwjsAnalyzer(NsfwjsConfig config) : config_(std::move(config)) {}

  bool open() override {
    if (session_) return true;
    try {
      if (config_.threads < 1) throw std::invalid_argument("NSFWJS threads must be positive");
      if (config_.max_regions < 1 || config_.max_regions > kMaximumRegions) {
        throw std::invalid_argument("NSFWJS regions must be between 1 and 9");
      }
      Ort::SessionOptions options;
      options.SetIntraOpNumThreads(config_.threads);
      options.SetInterOpNumThreads(1);
      options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      session_ = std::make_unique<Ort::Session>(env_, config_.model_path.c_str(), options);
      if (session_->GetInputCount() != 1 || session_->GetOutputCount() != 1) {
        throw std::runtime_error("NSFWJS model must have one input and one output");
      }
      const auto input_type = session_->GetInputTypeInfo(0);
      const auto output_type = session_->GetOutputTypeInfo(0);
      const auto input = input_type.GetTensorTypeAndShapeInfo();
      const auto output = output_type.GetTensorTypeAndShapeInfo();
      if (input.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          input.GetShape() != std::vector<std::int64_t>({1, kSize, kSize, 3}) ||
          output.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          output.GetShape() != std::vector<std::int64_t>({1, 5})) {
        throw std::runtime_error("NSFWJS model requires FP32 NHWC [1,224,224,3] -> [1,5]");
      }
      Ort::AllocatorWithDefaultOptions allocator;
      const auto metadata = session_->GetModelMetadata();
      const auto labels = metadata.LookupCustomMetadataMapAllocated("nsfwjs.classes", allocator);
      const auto preprocessing = metadata.LookupCustomMetadataMapAllocated("nsfwjs.input", allocator);
      if (!labels || std::string(labels.get()) != "Drawing,Hentai,Neutral,Porn,Sexy" ||
          !preprocessing || std::string(preprocessing.get()) != "rgb_nhwc_float32_0_1_align_corners") {
        throw std::runtime_error("NSFWJS model metadata is missing or incompatible; use tools/export_nsfwjs.py");
      }
      input_name_ = session_->GetInputNameAllocated(0, allocator).get();
      output_name_ = session_->GetOutputNameAllocated(0, allocator).get();
      K230_LOG_INFO("nsfwjs") << "loaded " << config_.model_path << " threads=" << config_.threads;
      return true;
    } catch (const std::exception& e) {
      session_.reset();
      K230_LOG_ERROR("nsfwjs") << e.what();
      return false;
    }
  }

  Scores analyze(const SyncedSample& sample) override {
    if (!session_) throw std::runtime_error("NSFWJS analyzer is not open");
    const auto start = std::chrono::steady_clock::now();
    const auto regions = nsfwjs_regions(sample.frame, config_.max_regions);
    const std::uint32_t region_index = next_region_++ % regions.size();
    auto input = nsfwjs_input(sample.frame, regions[region_index]);
    const std::array<std::int64_t, 4> shape{1, kSize, kSize, 3};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
    const char* inputs[] = {input_name_.c_str()};
    const char* outputs[] = {output_name_.c_str()};
    auto result = session_->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    if (!result[0].IsTensor() || result[0].GetTensorTypeAndShapeInfo().GetShape() !=
                                   std::vector<std::int64_t>({1, 5})) {
      throw std::runtime_error("NSFWJS returned an invalid output shape");
    }
    const float* probabilities = result[0].GetTensorData<float>();
    float sum = 0.0f;
    for (std::size_t i = 0; i < 5; ++i) {
      if (!std::isfinite(probabilities[i]) || probabilities[i] < 0.0f || probabilities[i] > 1.0f) {
        throw std::runtime_error("NSFWJS returned invalid probabilities");
      }
      sum += probabilities[i];
    }
    if (std::abs(sum - 1.0f) > 0.001f) throw std::runtime_error("NSFWJS output is not Softmax");
    Scores scores;
    scores.nsfwjs = NsfwjsScores{probabilities[0], probabilities[1], probabilities[2],
                               probabilities[3], probabilities[4]};
    scores.nudity = std::min(1.0f, scores.nsfwjs->porn + scores.nsfwjs->hentai);
    scores.audio_level = HeuristicAnalyzer::rms(sample.audio);
    scores.analysis_region = region_index;
    scores.analysis_regions = static_cast<std::uint32_t>(regions.size());
    scores.analysis_layout = static_cast<std::uint64_t>(sample.frame.width) << 32 | sample.frame.height;
    scores.analysis_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return scores;
  }

  const char* name() const override { return "nsfwjs-mobilenet-v2-onnx"; }

 private:
  NsfwjsConfig config_;
  Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "nsfwjs"};
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_;
  std::string output_name_;
  std::uint64_t next_region_ = 0;
};

}  // namespace
#endif

std::unique_ptr<Analyzer> make_nsfwjs_analyzer(const NsfwjsConfig& config) {
#ifdef K230_HAS_ONNX
  return std::make_unique<NsfwjsAnalyzer>(config);
#else
  (void)config;
  K230_LOG_ERROR("nsfwjs") << "rebuild the PC target with -DK230_ENABLE_ONNX=ON and -DONNXRUNTIME_ROOT=...";
  return nullptr;
#endif
}

}  // namespace k230::inspector
