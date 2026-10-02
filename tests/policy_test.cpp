#include "k230/inspector/policy.hpp"

#include <gtest/gtest.h>

using namespace k230;
using namespace k230::inspector;

namespace {
PolicyConfig cfg() {
  PolicyConfig c;
  c.warn_threshold = 0.6f;
  c.block_threshold = 0.85f;
  c.confirm_frames = 3;
  c.cooldown_us = 1'000'000;
  return c;
}
Scores nud(float v) {
  Scores s;
  s.nudity = v;
  return s;
}
}  // namespace

TEST(Policy, SafeFramesLog) {
  Policy p(cfg());
  for (int i = 0; i < 10; ++i) {
    auto v = p.evaluate(i * 100'000, nud(0.1f));
    EXPECT_EQ(v.action, Action::Log);
    EXPECT_EQ(v.category, Category::Safe);
    EXPECT_EQ(v.sequence, static_cast<std::uint32_t>(i + 1));
  }
}

TEST(Policy, SingleSpikeDoesNotEscalate) {
  Policy p(cfg());
  EXPECT_EQ(p.evaluate(0, nud(0.99f)).action, Action::Log);
  EXPECT_EQ(p.evaluate(100'000, nud(0.1f)).action, Action::Log);
  EXPECT_EQ(p.evaluate(200'000, nud(0.99f)).action, Action::Log);
  EXPECT_EQ(p.evaluate(300'000, nud(0.99f)).action, Action::Log);
  EXPECT_EQ(p.evaluate(400'000, nud(0.99f)).action, Action::Block);  // third consecutive
}

TEST(Policy, ConfirmsEachScreenRegionIndependently) {
  Policy p(cfg());
  auto risky = nud(0.9f);
  auto safe = nud(0.1f);
  risky.analysis_layout = safe.analysis_layout = 42;
  risky.analysis_region = 2;
  safe.analysis_region = 5;
  EXPECT_EQ(p.evaluate(0, risky).action, Action::Log);
  EXPECT_EQ(p.evaluate(100'000, safe).action, Action::Log);
  EXPECT_EQ(p.evaluate(200'000, risky).action, Action::Log);
  EXPECT_EQ(p.evaluate(300'000, safe).action, Action::Log);
  EXPECT_EQ(p.evaluate(400'000, risky).action, Action::Block);
}

TEST(Policy, LayoutChangeResetsRegionConfirmation) {
  Policy p(cfg());
  auto scores = nud(0.9f);
  scores.analysis_region = 2;
  scores.analysis_layout = 42;
  EXPECT_EQ(p.evaluate(0, scores).action, Action::Log);
  EXPECT_EQ(p.evaluate(100'000, scores).action, Action::Log);
  scores.analysis_layout = 84;
  EXPECT_EQ(p.evaluate(200'000, scores).action, Action::Log);
  EXPECT_EQ(p.evaluate(300'000, scores).action, Action::Log);
  EXPECT_EQ(p.evaluate(400'000, scores).action, Action::Block);
}

TEST(Policy, ConsecutiveHighFramesBlockThenCooldown) {
  Policy p(cfg());
  EXPECT_EQ(p.evaluate(0, nud(0.9f)).action, Action::Log);
  EXPECT_EQ(p.evaluate(100'000, nud(0.9f)).action, Action::Log);
  auto v = p.evaluate(200'000, nud(0.9f));
  EXPECT_EQ(v.action, Action::Block);
  EXPECT_EQ(v.category, Category::Nudity);
  EXPECT_FLOAT_EQ(v.confidence, 0.9f);

  // still high, but silenced for 1 s
  for (int i = 3; i < 12; ++i) EXPECT_EQ(p.evaluate(i * 100'000, nud(0.9f)).action, Action::Log) << i;
  // cooldown over at 1.2 s and the content is still sustained -> re-issue
  EXPECT_EQ(p.evaluate(1'200'000, nud(0.9f)).action, Action::Block);
}

TEST(Policy, WarnBandProducesWarn) {
  Policy p(cfg());
  EXPECT_EQ(p.evaluate(0, nud(0.7f)).action, Action::Log);
  EXPECT_EQ(p.evaluate(1, nud(0.7f)).action, Action::Log);
  auto v = p.evaluate(2, nud(0.7f));
  EXPECT_EQ(v.action, Action::Warn);
  EXPECT_EQ(v.category, Category::Nudity);
}

TEST(Policy, MostSevereCategoryWins) {
  Policy p(cfg());
  Scores s;
  s.nudity = 0.7f;     // warn band
  s.violence = 0.95f;  // block band
  p.evaluate(0, s);
  p.evaluate(1, s);
  auto v = p.evaluate(2, s);
  EXPECT_EQ(v.action, Action::Block);
  EXPECT_EQ(v.category, Category::Violence);
}

TEST(Policy, EscalatedActionIsConfigurable) {
  PolicyConfig c = cfg();
  c.escalated_action = Action::Delete;
  Policy p(c);
  p.evaluate(0, nud(0.9f));
  p.evaluate(1, nud(0.9f));
  EXPECT_EQ(p.evaluate(2, nud(0.9f)).action, Action::Delete);
}
