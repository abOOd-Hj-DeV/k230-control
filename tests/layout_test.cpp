#include "k230/layout.hpp"
#include "k230/inspector/fast_vision.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>
#include <fstream>
#include <iterator>
#include <condition_variable>
#include <stdexcept>

using namespace k230;
using namespace k230::inspector;

namespace {
LayoutSnapshot snapshot() {
  LayoutSnapshot s;
  s.session = 7;
  s.sequence = 9;
  s.sampled_at_us = 1'000'000;
  s.completed_at_us = 1'005'000;
  s.valid_from_us = 990'000;
  s.width = 1080;
  s.height = 2400;
  s.window_id = 42;
  s.package = "dev.example";
  s.nodes = {{100, 200, 900, 1000, 1, 1}};
  return s;
}
std::vector<std::uint8_t> golden() {
  std::ifstream file(K230_LAYOUT_GOLDEN, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
  for (unsigned int i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
}

TEST(LayoutWire, DecodesFixtureWrittenByKotlinEncoder) {
  const auto bytes = golden();
  ASSERT_EQ(bytes.size(), 115u);
  EXPECT_EQ(layout_payload_size(bytes.data()), 111u);
  auto s = decode_layout(bytes.data() + 4, bytes.size() - 4);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->session, 7u);
  EXPECT_EQ(s->sequence, 9u);
  EXPECT_EQ(s->sampled_at_us, 1'000'000);
  EXPECT_EQ(s->completed_at_us, 1'005'000);
  EXPECT_EQ(s->valid_from_us, 990'000);
  EXPECT_EQ(s->width, 1080u);
  EXPECT_EQ(s->height, 2400u);
  EXPECT_EQ(s->package, "dev.example");
  ASSERT_EQ(s->nodes.size(), 1u);
  EXPECT_EQ(s->nodes[0].right, 900u);
  EXPECT_EQ(s->nodes[0].id, 1u);
}

TEST(LayoutWire, RejectsMalformedLengthsVersionsDimensionsAndBounds) {
  auto bytes = golden();
  ASSERT_EQ(bytes.size(), 115u);
  for (const auto at : {4u, 8u, 10u, 16u, 24u, 52u, 56u, 60u, 64u, 72u, 76u, 91u, 107u, 111u}) {
    auto bad = bytes;
    put32(bad, at, 0xffffffff);
    EXPECT_FALSE(decode_layout(bad.data() + 4, bad.size() - 4)) << at;
  }
  EXPECT_FALSE(decode_layout(bytes.data() + 4, bytes.size() - 5));
  EXPECT_FALSE(decode_layout(bytes.data(), kLayoutHeaderSize - 1));
  EXPECT_FALSE(decode_layout(bytes.data(), kLayoutMaxPayload + 1));
}

TEST(LayoutCache, RejectsDuplicateSequenceSessionAndExpiredFutureLayouts) {
  LayoutCache cache;
  auto s = snapshot();
  ASSERT_TRUE(cache.push(s));
  EXPECT_FALSE(cache.push(s));
  s.sequence++;
  s.session++;
  EXPECT_FALSE(cache.push(s));
  EXPECT_FALSE(cache.match(1'000'000));
  EXPECT_TRUE(cache.match(1'010'000));
  EXPECT_FALSE(cache.match(1'300'000));
  cache.clear();
  EXPECT_FALSE(cache.match(1'010'000));
  EXPECT_TRUE(cache.push(s));
}

TEST(LayoutWire, DecodesActualAndroidServiceResponseWithThreeImageViews) {
  std::ifstream file(K230_LAYOUT_LIVE, std::ios::binary);
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
  ASSERT_GE(bytes.size(), 4u + kLayoutHeaderSize);
  const auto s = decode_layout(bytes.data() + 4, bytes.size() - 4);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->width, 1080u);
  EXPECT_EQ(s->height, 2400u);
  EXPECT_EQ(s->package, "dev.k230.mentor_app");
  EXPECT_EQ(std::count_if(s->nodes.begin(), s->nodes.end(), [](const auto& n) { return n.kind == 1; }), 3);
}

TEST(LayoutCache, EventBarrierPreventsUseOfOldCoordinatesAndSequenceGapsClearHistory) {
  LayoutCache cache;
  auto s = snapshot();
  ASSERT_TRUE(cache.push(s));
  s.sequence++;
  s.sampled_at_us = s.completed_at_us = s.valid_from_us = 1'100'000;
  s.flags = kLayoutInvalidate;
  s.nodes.clear();
  ASSERT_TRUE(cache.push(s));
  EXPECT_TRUE(cache.match(1'080'000));
  EXPECT_FALSE(cache.match(1'110'000));
  s.sequence++;
  s.flags = 0;
  s.sampled_at_us = 1'120'000;
  s.completed_at_us = 1'130'000;
  ASSERT_TRUE(cache.push(s));
  EXPECT_FALSE(cache.match(1'125'000));
  EXPECT_TRUE(cache.match(1'140'000));
  s.sequence += 2;
  s.sampled_at_us = s.completed_at_us = 1'150'000;
  ASSERT_TRUE(cache.push(s));
  EXPECT_FALSE(cache.match(1'140'000));
}

TEST(LayoutCache, InFlightPeriodicRefreshDoesNotInvalidateSameLayout) {
  LayoutCache cache;
  auto s = snapshot();
  ASSERT_TRUE(cache.push(s));
  s.sequence++;
  s.sampled_at_us = 1'020'000;
  s.completed_at_us = 1'025'000;
  ASSERT_TRUE(cache.push(s));
  const auto old = cache.match(1'022'000);
  ASSERT_TRUE(old);
  EXPECT_EQ(old->sequence, 9u);
}

TEST(LayoutTransform, ScreenBoundsAreAlreadyRotatedAndScaleAtAllFourRotations) {
  for (std::uint32_t rotation = 0; rotation < 4; ++rotation) {
    auto s = snapshot();
    s.rotation = rotation;
    if (rotation % 2) {
      std::swap(s.width, s.height);
      s.nodes[0] = {200, 100, 1000, 900, 1, 1};
    }
    const auto f = test::make_frame(1'010'000, s.width / 2, s.height / 2);
    const auto region = layout_region(s, s.nodes[0], f);
    ASSERT_TRUE(region);
    EXPECT_EQ(region->x, s.nodes[0].left / 2);
    EXPECT_EQ(region->y, s.nodes[0].top / 2);
    EXPECT_EQ(region->width, 400u);
    EXPECT_EQ(region->height, 400u);
  }
  auto s = snapshot();
  EXPECT_FALSE(layout_region(s, s.nodes[0], test::make_frame(1'010'000, 1200, 540)));
}

TEST(VisionRegions, ThreeImagesUseOneFrameAndPartialLayoutAddsOverlapFallback) {
  auto s = snapshot();
  s.nodes = {{0, 0, 1080, 400, 1, 1}, {0, 400, 1080, 800, 1, 2}, {0, 800, 1080, 1200, 1, 3}};
  const auto f = test::make_frame(1'010'000, 540, 1200);
  auto regions = vision_regions(f, s);
  EXPECT_EQ(regions.regions.size(), 4u);
  EXPECT_FALSE(regions.complete);
  s.flags = kLayoutPartial;
  regions = vision_regions(f, s);
  EXPECT_EQ(regions.regions.size(), 12u);
  EXPECT_EQ(vision_regions(f, std::nullopt).regions.size(), 9u);
}

TEST(Policy, SamePtsCannotProvideMultipleTemporalConfirmationsAndPartialCannotBeSafe) {
  PolicyConfig cfg;
  cfg.escalated_action = Action::Warn;
  Policy policy(cfg);
  Scores s;
  s.nudity = 0.9f;
  s.analysis_complete = false;
  EXPECT_EQ(policy.evaluate(100, s).action, Action::Log);
  EXPECT_EQ(policy.evaluate(100, s).action, Action::None);
  EXPECT_EQ(policy.evaluate(99, s).action, Action::None);
  EXPECT_EQ(policy.evaluate(200, s).action, Action::Log);
  EXPECT_EQ(policy.evaluate(300, s).action, Action::Warn);
  s.nudity = 0;
  EXPECT_EQ(policy.evaluate(400, s).category, Category::Unknown);
}

namespace {
struct WorkerState {
  std::mutex mutex;
  std::condition_variable changed;
  bool started = false, release = false;
  unsigned int active = 0, max_active = 0;
};
class TestRegionAnalyzer final : public RegionAnalyzer {
 public:
  explicit TestRegionAnalyzer(std::shared_ptr<WorkerState> state) : state_(std::move(state)) {}
  bool open() override { return true; }
  Scores analyze_region(const VideoFrame&, const ImageRegion& region) override {
    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      ++state_->active;
      state_->max_active = std::max(state_->max_active, state_->active);
      state_->started = true;
      state_->changed.notify_all();
      state_->changed.wait(lock, [&] { return state_->release; });
      --state_->active;
    }
    Scores s;
    s.nsfwjs = NsfwjsScores{};
    s.nudity = region.y >= 400 ? 0.95f : 0.1f;
    return s;
  }
 private:
  std::shared_ptr<WorkerState> state_;
};
}

TEST(FastVision, UsesTwoWorkersKeepsLatestPendingFrameAndTakesMaximumRisk) {
  auto state = std::make_shared<WorkerState>();
  auto cache = std::make_shared<LayoutCache>();
  auto s = snapshot();
  s.nodes = {{0, 0, 1080, 400, 1, 1}, {0, 400, 1080, 800, 1, 2}, {0, 800, 1080, 1200, 1, 3}};
  ASSERT_TRUE(cache->push(s));
  auto verdicts = std::make_shared<ipc::InProcessQueue<Verdict>>(8);
  PolicyConfig policy;
  policy.escalated_action = Action::Warn;
  FastVision vision([state] { return std::make_unique<TestRegionAnalyzer>(state); }, cache, policy, verdicts);
  std::vector<std::int64_t> pts;
  vision.set_observer([&](const auto& frame, const auto& scores, const auto& verdict) {
    pts.push_back(frame.pts_us);
    EXPECT_FLOAT_EQ(scores.nudity, 0.95f);
    EXPECT_EQ(scores.analysis_regions, 4u);
    EXPECT_EQ(scores.source_region, 3u);
    EXPECT_NE(verdict.category, Category::Safe);
  });
  ASSERT_TRUE(vision.start());
  vision.submit(test::make_frame(1'010'000, 540, 1200));
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    ASSERT_TRUE(state->changed.wait_for(lock, std::chrono::seconds(2), [&] { return state->active == 2; }));
  }
  vision.submit(test::make_frame(1'020'000, 540, 1200));
  vision.submit(test::make_frame(1'030'000, 540, 1200));
  vision.submit(test::make_frame(1'040'000, 540, 1200));
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->release = true;
    state->changed.notify_all();
  }
  vision.finish();
  EXPECT_EQ(pts, (std::vector<std::int64_t>{1'010'000, 1'040'000}));
  EXPECT_EQ(state->max_active, 2u);
  EXPECT_EQ(vision.stats().dropped, 2u);
  EXPECT_EQ(vision.stats().analyzed, 2u);
  EXPECT_EQ(vision.stats().warnings, 0u);
}
