#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "k230/inspector/nsfwjs_analyzer.hpp"
#include "k230/inspector/pipeline.hpp"

using namespace k230;
using namespace k230::inspector;

namespace {

VideoFrame black_frame(std::int64_t pts_us = 2'300'000) {
  VideoFrame frame;
  frame.width = 3;
  frame.height = 5;
  frame.pts_us = pts_us;
  frame.data.assign(frame.luma_size() + 2 * 2 * 3, 128);
  std::fill_n(frame.data.begin(), frame.luma_size(), 16);
  return frame;
}

class TestDecoder final : public VideoDecoder {
 public:
  bool open(CodecId) override { return true; }
  bool decode(const MediaPacket& packet, std::vector<VideoFrame>& out) override {
    out.push_back(black_frame(packet.pts_us));
    return true;
  }
  void flush(std::vector<VideoFrame>&) override {}
  const char* name() const override { return "test-decoder"; }
};

class FailingAnalyzer final : public Analyzer {
 public:
  explicit FailingAnalyzer(bool opens) : opens_(opens) {}
  bool open() override { return opens_; }
  Scores analyze(const SyncedSample&) override { throw std::runtime_error("test inference failure"); }
  const char* name() const override { return "failing-analyzer"; }
 private:
  bool opens_;
};

std::shared_ptr<ipc::InProcessQueue<MediaPacket>> packets(int count) {
  auto queue = std::make_shared<ipc::InProcessQueue<MediaPacket>>(8);
  for (int i = 0; i < count; ++i) {
    MediaPacket packet;
    packet.stream = StreamType::Video;
    packet.codec = CodecId::H264;
    packet.pts_us = 2'300'000 + i * 100'000;
    queue->push(std::move(packet));
  }
  queue->close();
  return queue;
}

}  // namespace

TEST(NsfwjsInput, RgbOrderAndNormalization) {
  VideoFrame frame;
  frame.width = frame.height = 1;
  frame.data = {82, 90, 239};
  const auto input = nsfwjs_input(frame);
  ASSERT_EQ(input.size(), 224u * 224u * 3u);
  for (std::size_t i = 0; i < input.size(); i += 3) {
    ASSERT_FLOAT_EQ(input[i], 254.0f / 255.0f);
    ASSERT_FLOAT_EQ(input[i + 1], 1.0f / 255.0f);
    ASSERT_FLOAT_EQ(input[i + 2], 0.0f);
  }
}

TEST(NsfwjsInput, BilinearResizeAlignsCorners) {
  VideoFrame frame;
  frame.width = frame.height = 2;
  frame.data = {16, 235, 235, 16, 128, 128};
  const auto input = nsfwjs_input(frame);
  EXPECT_FLOAT_EQ(input.front(), 0.0f);
  EXPECT_FLOAT_EQ(input[223 * 3], 1.0f);
  EXPECT_FLOAT_EQ(input[223 * 224 * 3], 1.0f);
  EXPECT_FLOAT_EQ(input.back(), 0.0f);
  const float x = 100.0f / 223.0f;
  const float y = 70.0f / 223.0f;
  EXPECT_NEAR(input[(70 * 224 + 100) * 3], x + y - 2 * x * y, 1e-6f);
}

TEST(NsfwjsInput, OddSizedI420AndNv12Match) {
  auto planar = black_frame();
  for (std::size_t i = 0; i < planar.data.size(); ++i) planar.data[i] = (i * 31) % 256;
  auto interleaved = planar;
  interleaved.format = PixelFormat::NV12;
  const std::size_t luma = planar.luma_size();
  const std::size_t chroma = 2 * 3;
  for (std::size_t i = 0; i < chroma; ++i) {
    interleaved.data[luma + 2 * i] = planar.data[luma + i];
    interleaved.data[luma + 2 * i + 1] = planar.data[luma + chroma + i];
  }
  EXPECT_EQ(nsfwjs_input(planar), nsfwjs_input(interleaved));
}

TEST(NsfwjsInput, RejectsEmptyOversizedAndTruncatedFrames) {
  EXPECT_THROW(nsfwjs_input(VideoFrame{}), std::invalid_argument);
  auto frame = black_frame();
  frame.data.pop_back();
  EXPECT_THROW(nsfwjs_input(frame), std::invalid_argument);
  frame.width = 4097;
  EXPECT_THROW(nsfwjs_input(frame), std::invalid_argument);
}

TEST(NsfwjsInput, GeneratesOverlappingPortraitRegionsWithinBounds) {
  VideoFrame frame;
  frame.width = 358;
  frame.height = 800;
  const auto regions = nsfwjs_regions(frame, 9);
  ASSERT_EQ(regions.size(), 9u);
  EXPECT_EQ(regions.front().x, 0u);
  EXPECT_EQ(regions.front().y, 0u);
  EXPECT_EQ(regions.front().width, 358u);
  EXPECT_EQ(regions.front().height, 800u);
  for (std::size_t i = 1; i < regions.size(); ++i) {
    EXPECT_EQ(regions[i].x, 0u);
    EXPECT_EQ(regions[i].width, 358u);
    EXPECT_EQ(regions[i].height, 286u);
    EXPECT_LE(regions[i].y + regions[i].height, frame.height);
    if (i > 1) {
      EXPECT_GT(regions[i].y, regions[i - 1].y);
    }
  }
  EXPECT_EQ(regions[3].y, 147u);
  EXPECT_EQ(regions.back().y, 514u);
  EXPECT_EQ(nsfwjs_regions(frame, 1).size(), 1u);
  EXPECT_EQ(nsfwjs_regions(black_frame(), 9).size(), 1u);
  EXPECT_THROW(nsfwjs_regions(frame, 0), std::invalid_argument);
  EXPECT_THROW(nsfwjs_regions(frame, 10), std::invalid_argument);
}

TEST(NsfwjsInput, RegionResizeUsesRegionCorners) {
  VideoFrame frame;
  frame.width = frame.height = 4;
  frame.data = {
      16, 16, 16, 16,
      16, 16, 16, 16,
      16, 16, 235, 235,
      16, 16, 235, 235,
      128, 128, 128, 128,
      128, 128, 128, 128,
  };
  const auto input = nsfwjs_input(frame, NsfwjsRegion{2, 2, 2, 2});
  ASSERT_EQ(input.size(), 224u * 224u * 3u);
  for (std::size_t i = 0; i < input.size(); ++i) EXPECT_FLOAT_EQ(input[i], 1.0f);
  EXPECT_THROW(nsfwjs_input(frame, NsfwjsRegion{3, 3, 2, 2}), std::invalid_argument);
}

TEST(NsfwjsPipeline, RequestedAnalyzerCannotFallBackToHeuristic) {
  PipelineConfig config;
  config.sync.audio_enabled = false;
  config.allow_analyzer_fallback = false;
  auto verdicts = std::make_shared<ipc::InProcessQueue<Verdict>>(8);
  InspectorPipeline pipeline(config, packets(1), verdicts, std::make_unique<TestDecoder>(),
                             std::make_unique<FailingAnalyzer>(false));
  EXPECT_FALSE(pipeline.run());
  EXPECT_EQ(pipeline.stats().samples_analyzed, 0u);
  EXPECT_TRUE(verdicts->closed());
  EXPECT_EQ(verdicts->size(), 0u);
}

TEST(NsfwjsPipeline, InferenceFailureReturnsErrorWithoutSafeVerdict) {
  PipelineConfig config;
  config.sync.audio_enabled = false;
  config.allow_analyzer_fallback = false;
  auto verdicts = std::make_shared<ipc::InProcessQueue<Verdict>>(8);
  InspectorPipeline pipeline(config, packets(1), verdicts, std::make_unique<TestDecoder>(),
                             std::make_unique<FailingAnalyzer>(true));
  EXPECT_FALSE(pipeline.run());
  EXPECT_EQ(pipeline.stats().samples_analyzed, 0u);
  EXPECT_TRUE(verdicts->closed());
  EXPECT_EQ(verdicts->size(), 0u);
}

#ifdef K230_HAS_ONNX
TEST(NsfwjsModel, MatchesOriginalTfjsBlackImageAndKeepsAudioDiagnostic) {
  NsfwjsConfig config;
  config.model_path = K230_TEST_NSFWJS_MODEL;
  auto analyzer = make_nsfwjs_analyzer(config);
  ASSERT_NE(analyzer, nullptr);
  ASSERT_TRUE(analyzer->open());
  ASSERT_TRUE(analyzer->open());
  SyncedSample sample;
  sample.frame = black_frame();
  sample.audio = {-16384, 16384};
  const auto scores = analyzer->analyze(sample);
  ASSERT_TRUE(scores.nsfwjs);
  const auto& nsfw = *scores.nsfwjs;
  EXPECT_NEAR(nsfw.drawing, 0.788308978f, 2e-5f);
  EXPECT_NEAR(nsfw.hentai, 0.026247092f, 2e-5f);
  EXPECT_NEAR(nsfw.neutral, 0.173371479f, 2e-5f);
  EXPECT_NEAR(nsfw.porn, 0.010009553f, 2e-5f);
  EXPECT_NEAR(nsfw.sexy, 0.002062896f, 2e-5f);
  EXPECT_NEAR(nsfw.drawing + nsfw.hentai + nsfw.neutral + nsfw.porn + nsfw.sexy, 1.0f, 1e-6f);
  EXPECT_FLOAT_EQ(scores.nudity, nsfw.porn + nsfw.hentai);
  EXPECT_FLOAT_EQ(scores.violence, 0.0f);
  EXPECT_FLOAT_EQ(scores.profanity, 0.0f);
  EXPECT_FLOAT_EQ(scores.audio_level, 0.5f);
  EXPECT_GT(scores.analysis_ms, 0.0);
}

TEST(NsfwjsModel, MissingOrCorruptModelAndInvalidThreadsFailToOpen) {
  NsfwjsConfig config;
  config.model_path = std::string(K230_TEST_NSFWJS_MODEL) + ".missing";
  EXPECT_FALSE(make_nsfwjs_analyzer(config)->open());
  config.model_path = __FILE__;
  EXPECT_FALSE(make_nsfwjs_analyzer(config)->open());
  config.model_path = K230_TEST_NSFWJS_MODEL;
  config.threads = 0;
  EXPECT_FALSE(make_nsfwjs_analyzer(config)->open());
  config.threads = 1;
  config.max_regions = 10;
  EXPECT_FALSE(make_nsfwjs_analyzer(config)->open());
}

TEST(NsfwjsModel, RotatesThroughConfiguredScreenRegions) {
  NsfwjsConfig config;
  config.model_path = K230_TEST_NSFWJS_MODEL;
  config.max_regions = 4;
  auto analyzer = make_nsfwjs_analyzer(config);
  ASSERT_TRUE(analyzer->open());
  SyncedSample sample;
  sample.frame.width = 320;
  sample.frame.height = 640;
  sample.frame.data.assign(sample.frame.luma_size() * 3 / 2, 128);
  std::fill_n(sample.frame.data.begin(), sample.frame.luma_size(), 16);
  for (std::uint32_t expected = 0; expected < 4; ++expected) {
    const auto scores = analyzer->analyze(sample);
    EXPECT_EQ(scores.analysis_region, expected);
    EXPECT_EQ(scores.analysis_regions, 4u);
  }
  EXPECT_EQ(analyzer->analyze(sample).analysis_region, 0u);
}

TEST(NsfwjsPipeline, PreservesPhonePtsAndTemporalConfirmationWithRealModel) {
  PipelineConfig config;
  config.sync.audio_enabled = false;
  config.allow_analyzer_fallback = false;
  config.policy.warn_threshold = 0.01f;
  config.policy.block_threshold = 0.02f;
  config.policy.escalated_action = Action::Warn;
  NsfwjsConfig model;
  model.model_path = K230_TEST_NSFWJS_MODEL;
  auto verdicts = std::make_shared<ipc::InProcessQueue<Verdict>>(8);
  InspectorPipeline pipeline(config, packets(3), verdicts, std::make_unique<TestDecoder>(),
                             make_nsfwjs_analyzer(model));
  std::vector<std::int64_t> pts;
  pipeline.set_observer([&](const SyncedSample& sample, const Scores& scores, const Verdict& verdict) {
    EXPECT_TRUE(scores.nsfwjs);
    EXPECT_EQ(verdict.pts_us, sample.frame.pts_us);
    pts.push_back(verdict.pts_us);
  });
  ASSERT_TRUE(pipeline.run());
  EXPECT_EQ(pts, (std::vector<std::int64_t>{2'300'000, 2'400'000, 2'500'000}));
  EXPECT_EQ(pipeline.stats().samples_analyzed, 3u);
  ASSERT_EQ(verdicts->size(), 3u);
  EXPECT_EQ(verdicts->pop(std::chrono::milliseconds(0))->action, Action::Log);
  EXPECT_EQ(verdicts->pop(std::chrono::milliseconds(0))->action, Action::Log);
  const auto warning = verdicts->pop(std::chrono::milliseconds(0));
  ASSERT_TRUE(warning);
  EXPECT_EQ(warning->action, Action::Warn);
  EXPECT_EQ(warning->category, Category::Nudity);
}
#else
TEST(NsfwjsModel, UnavailableBackendDoesNotSubstituteHeuristic) {
  EXPECT_EQ(make_nsfwjs_analyzer(NsfwjsConfig{}), nullptr);
}
#endif
