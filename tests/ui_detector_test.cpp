#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

#include "k230/inspector/fast_vision.hpp"
#include "test_helpers.hpp"

using namespace k230;
using namespace k230::inspector;

namespace {
std::vector<float> predictions() { return std::vector<float>((kUiClasses + 4) * kUiBoxes); }

void box(std::vector<float>& output, std::size_t index, float x, float y, float w, float h,
         std::size_t label = 9, float confidence = 0.9f) {
  output[index] = x;
  output[kUiBoxes + index] = y;
  output[2 * kUiBoxes + index] = w;
  output[3 * kUiBoxes + index] = h;
  output[(4 + label) * kUiBoxes + index] = confidence;
}

class TestDetector final : public RegionDetector {
 public:
  explicit TestDetector(bool small_only) : small_only_(small_only) {}
  bool open() override { return true; }
  UiDetections detect(const VideoFrame& frame) override {
    auto output = predictions();
    if (!small_only_) box(output, 0, 200, 250, 200, 100);
    box(output, 1, 400, 250, 32, 32);
    return ui_regions(frame, output, UiDetectorConfig{});
  }
 private:
  bool small_only_;
};

class CountingAnalyzer final : public RegionAnalyzer {
 public:
  explicit CountingAnalyzer(std::atomic<unsigned>& calls) : calls_(calls) {}
  bool open() override { return true; }
  Scores analyze_region(const VideoFrame& frame, const ImageRegion& region) override {
    ++calls_;
    EXPECT_EQ(frame.pts_us, 123'456);
    EXPECT_EQ(region.x, 100u);
    EXPECT_EQ(region.y, 40u);
    EXPECT_EQ(region.width, 200u);
    EXPECT_EQ(region.height, 100u);
    Scores scores;
    scores.nsfwjs = NsfwjsScores{};
    return scores;
  }
 private:
  std::atomic<unsigned>& calls_;
};
}  // namespace

TEST(UiInput, RgbChannelsAndLetterboxPaddingMatchForOddI420AndNv12) {
  VideoFrame frame;
  frame.width = 5;
  frame.height = 7;
  frame.data.assign(35 + 24, 128);
  std::fill_n(frame.data.begin(), 35, 82);
  std::fill_n(frame.data.begin() + 35, 12, 90);
  std::fill_n(frame.data.begin() + 47, 12, 240);
  const auto planar = ui_input(frame);
  for (std::size_t i = 0; i < 12; ++i) {
    frame.data[35 + i * 2] = 90;
    frame.data[36 + i * 2] = 240;
  }
  frame.format = PixelFormat::NV12;
  EXPECT_EQ(planar, ui_input(frame));
  constexpr std::size_t plane = 640 * 640, center = 320 * 640 + 320;
  EXPECT_FLOAT_EQ(planar[0], 114.0f / 255.0f);
  EXPECT_FLOAT_EQ(planar[center], 1);
  EXPECT_FLOAT_EQ(planar[plane + center], 1.0f / 255.0f);
  EXPECT_FLOAT_EQ(planar[2 * plane + center], 0);
  frame.data.clear();
  EXPECT_THROW(ui_input(frame), std::invalid_argument);
  frame.width = 0;
  EXPECT_THROW(ui_input(frame), std::invalid_argument);
}

TEST(UiRegions, RemovesPaddingSuppressesDuplicatesAndIgnoresNonMediaAndClippedTinyBoxes) {
  auto output = predictions();
  box(output, 0, 310, 300, 200, 160);
  box(output, 1, 310, 300, 200, 160, 0, 0.8f);
  box(output, 2, 310, 300, 200, 160, 8, 0.99f);
  output[(4 + 9) * kUiBoxes + 2] = 0.95f;
  box(output, 3, 180, 20, 80, 100);
  const auto detected = ui_regions(test::make_frame(100, 320, 640), output, UiDetectorConfig{});
  ASSERT_EQ(detected.regions.size(), 1u);
  EXPECT_EQ(detected.regions[0].x, 50u);
  EXPECT_EQ(detected.regions[0].y, 220u);
  EXPECT_EQ(detected.regions[0].width, 200u);
  EXPECT_EQ(detected.regions[0].height, 160u);
  EXPECT_EQ(detected.ignored_small, 1u);
}

TEST(UiRegions, MinimumSideAndAreaApplyBeforeClassificationAndAreConfigurable) {
  auto output = predictions();
  box(output, 0, 100, 100, 63, 100);
  box(output, 1, 300, 100, 64, 100);
  UiDetectorConfig config;
  const auto square = ui_regions(test::make_frame(100, 640, 640), output, config);
  ASSERT_EQ(square.regions.size(), 1u);
  EXPECT_EQ(square.ignored_small, 1u);
  box(output, 0, 300, 100, 32, 32);
  std::fill(output.begin() + 1, output.begin() + kUiBoxes, 0);
  output[(4 + 9) * kUiBoxes + 1] = 0;
  const auto tall = test::make_frame(100, 640, 1280);
  EXPECT_TRUE(ui_regions(tall, output, config).regions.empty());
  config.min_area = 0;
  EXPECT_EQ(ui_regions(tall, output, config).regions.size(), 1u);
  config.min_side = 65;
  EXPECT_TRUE(ui_regions(tall, output, config).regions.empty());
}

TEST(UiRegions, RejectsMalformedOutputsAndConfiguration) {
  auto output = predictions();
  auto frame = test::make_frame(100, 640, 640);
  EXPECT_THROW(ui_regions(frame, {}, UiDetectorConfig{}), std::invalid_argument);
  output[4 * kUiBoxes] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(ui_regions(frame, output, UiDetectorConfig{}), std::runtime_error);
  auto config = UiDetectorConfig{};
  config.min_side = 0;
  EXPECT_THROW(ui_regions(frame, predictions(), config), std::invalid_argument);
  config.min_side = 64;
  config.min_area = 2;
  EXPECT_THROW(ui_regions(frame, predictions(), config), std::invalid_argument);
}

TEST(FastVisionUi, ClassifiesOnlyLargeDetectionsOnTheirOwnFrameAndNeverFallsBackToStrips) {
  for (const bool small_only : {false, true}) {
    std::atomic<unsigned> calls{0};
    auto verdicts = std::make_shared<ipc::InProcessQueue<Verdict>>(8);
    FastVision vision([&] { return std::make_unique<CountingAnalyzer>(calls); },
                      std::make_unique<TestDetector>(small_only), PolicyConfig{}, verdicts);
    bool observed = false;
    vision.set_observer([&](const auto& frame, const auto& scores, const auto& verdict) {
      observed = true;
      EXPECT_EQ(frame.pts_us, 123'456);
      EXPECT_EQ(scores.analysis_regions, small_only ? 0u : 1u);
      EXPECT_EQ(scores.crop.has_value(), !small_only);
      EXPECT_FALSE(scores.analysis_complete);
      EXPECT_EQ(verdict.category, Category::Unknown);
    });
    ASSERT_TRUE(vision.start());
    vision.submit(test::make_frame(123'456, 640, 320));
    vision.finish();
    EXPECT_TRUE(observed);
    EXPECT_EQ(calls, small_only ? 0u : 1u);
    EXPECT_EQ(vision.stats().analyzed, small_only ? 0u : 1u);
    EXPECT_EQ(vision.stats().skipped, small_only ? 1u : 0u);
    EXPECT_EQ(vision.stats().ignored_small, 1u);
    EXPECT_EQ(vision.stats().layout_misses, 0u);
  }
}

#ifdef K230_HAS_ONNX
TEST(UiModel, LoadsPinnedWeightsAndRejectsIncorrectModelOrInvalidLimits) {
  UiDetectorConfig config;
  config.model_path = K230_TEST_UI_MODEL;
  auto detector = make_ui_detector(config);
  ASSERT_TRUE(detector->open());
  const auto frame = test::make_frame(123'456, 640, 320);
  for (const auto& region : detector->detect(frame).regions) {
    EXPECT_GE(region.width, 64u);
    EXPECT_GE(region.height, 64u);
    EXPECT_LE(region.x + region.width, frame.width);
    EXPECT_LE(region.y + region.height, frame.height);
  }
  config.model_path = K230_TEST_NSFWJS_MODEL;
  EXPECT_FALSE(make_ui_detector(config)->open());
  config.min_side = 0;
  EXPECT_FALSE(make_ui_detector(config)->open());
}
#endif
