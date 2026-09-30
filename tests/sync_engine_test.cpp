#include "k230/inspector/sync_engine.hpp"

#include <gtest/gtest.h>

#include "test_helpers.hpp"

using namespace k230::inspector;
using namespace k230::test;

namespace {
SyncConfig cfg() {
  SyncConfig c;
  c.audio_before_us = 100'000;
  c.audio_after_us = 50'000;
  c.max_wait_us = 300'000;
  c.max_pending_frames = 4;
  return c;
}
}  // namespace

TEST(SyncEngine, FrameWaitsUntilAudioCoversWindow) {
  SyncEngine e(cfg());
  std::vector<SyncedSample> out;

  e.push_video(make_frame(1'000'000), 0);
  e.poll(0, out);
  EXPECT_TRUE(out.empty());  // no audio yet
  EXPECT_EQ(e.pending(), 1u);

  // audio up to 1.040 s: not enough (need until 1.050)
  for (int i = 0; i < 7; ++i) e.push_audio(make_pcm(900'000 + i * 20'000, 20'000, 5));
  e.poll(10'000, out);
  EXPECT_TRUE(out.empty());

  e.push_audio(make_pcm(1'040'000, 20'000, 5));  // now covers 1.060
  e.poll(20'000, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].frame.pts_us, 1'000'000);
  EXPECT_TRUE(out[0].audio_complete);
  EXPECT_EQ(out[0].audio_start_us, 900'000);
  EXPECT_EQ(out[0].audio_end_us, 1'050'000);
  EXPECT_EQ(out[0].audio_filled_us, 150'000);
  ASSERT_EQ(out[0].audio.size(), 150u * 48 * 2);
  EXPECT_EQ(out[0].audio[0], 5);
  EXPECT_EQ(e.pending(), 0u);
}

TEST(SyncEngine, UsesPhonePtsNotArrivalTime) {
  SyncEngine e(cfg());
  std::vector<SyncedSample> out;
  // Audio arrives long before the frame in wall time but is aligned by PTS.
  for (int i = 0; i < 20; ++i) e.push_audio(make_pcm(4'000'000 + i * 20'000, 20'000, static_cast<std::int16_t>(i)));
  e.push_video(make_frame(4'200'000), 99'000'000);  // wall time is irrelevant
  e.poll(99'000'000, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].audio_complete);
  // window 4.100..4.250 -> chunks 5..12
  EXPECT_EQ(out[0].audio.front(), 5);
  EXPECT_EQ(out[0].audio.back(), 12);
}

TEST(SyncEngine, ReleasesIncompleteAfterTimeout) {
  SyncEngine e(cfg());
  std::vector<SyncedSample> out;
  e.push_video(make_frame(2'000'000), 1'000'000);
  e.push_audio(make_pcm(1'900'000, 20'000, 3));  // partial
  e.poll(1'200'000, out);
  EXPECT_TRUE(out.empty());
  e.poll(1'000'000 + 300'000, out);  // max_wait reached
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].audio_complete);
  EXPECT_EQ(out[0].audio_filled_us, 20'000);
  EXPECT_EQ(out[0].audio.size(), 150u * 48 * 2);  // still full-length, zero padded
  EXPECT_EQ(e.stats().emitted_incomplete, 1u);
}

TEST(SyncEngine, GapInsideCoveredWindowIsStillIncomplete) {
  SyncEngine e(cfg());
  std::vector<SyncedSample> out;
  e.push_audio(make_pcm(900'000, 20'000, 1234));
  e.push_audio(make_pcm(1'040'000, 20'000, 1234));
  e.push_video(make_frame(1'000'000), 0);
  e.poll(0, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].audio_complete);
  EXPECT_EQ(out[0].audio_filled_us, 30'000);
  EXPECT_EQ(out[0].audio[20 * 48 * 2], 0);
  EXPECT_EQ(e.stats().emitted_incomplete, 1u);
}

TEST(SyncEngine, OverlappingChunksCannotHideGap) {
  SyncEngine e(cfg());
  std::vector<SyncedSample> out;
  e.push_audio(make_pcm(900'000, 60'000, 1234));
  e.push_audio(make_pcm(920'000, 40'000, 2345));
  e.push_audio(make_pcm(1'040'000, 20'000, 3456));
  e.push_video(make_frame(1'000'000), 0);
  e.poll(0, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].audio_complete);
  EXPECT_EQ(out[0].audio_filled_us, 70'000);
  EXPECT_EQ(out[0].audio[30 * 48 * 2], 2345);
  EXPECT_EQ(out[0].audio[80 * 48 * 2], 0);
}

TEST(SyncEngine, BackpressureReleasesOldestFrame) {
  SyncEngine e(cfg());
  std::vector<SyncedSample> out;
  for (int i = 0; i < 5; ++i) e.push_video(make_frame(i * 100'000), 0);
  e.poll(0, out);
  ASSERT_EQ(out.size(), 1u);  // 5 > max_pending_frames(4)
  EXPECT_EQ(out[0].frame.pts_us, 0);
  EXPECT_EQ(e.pending(), 4u);
  EXPECT_EQ(e.stats().emitted_backpressure, 1u);
}

TEST(SyncEngine, PreservesFrameOrder) {
  SyncEngine e(cfg());
  std::vector<SyncedSample> out;
  e.push_video(make_frame(1'000'000), 0);
  e.push_video(make_frame(1'100'000), 0);
  e.push_video(make_frame(1'200'000), 0);
  for (int i = 0; i < 30; ++i) e.push_audio(make_pcm(800'000 + i * 20'000, 20'000, 1));
  e.poll(0, out);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].frame.pts_us, 1'000'000);
  EXPECT_EQ(out[1].frame.pts_us, 1'100'000);
  EXPECT_EQ(out[2].frame.pts_us, 1'200'000);
}

TEST(SyncEngine, AudioDisabledReleasesImmediately) {
  SyncConfig c = cfg();
  c.audio_enabled = false;
  SyncEngine e(c);
  std::vector<SyncedSample> out;
  e.push_video(make_frame(5), 0);
  e.poll(0, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].audio.empty());
}

TEST(SyncEngine, FlushReleasesEverything) {
  SyncEngine e(cfg());
  std::vector<SyncedSample> out;
  e.push_video(make_frame(1), 0);
  e.push_video(make_frame(2), 0);
  e.flush(out);
  EXPECT_EQ(out.size(), 2u);
  EXPECT_EQ(e.pending(), 0u);
}
