#include "k230/inspector/pcm_ring.hpp"

#include <gtest/gtest.h>

#include "test_helpers.hpp"

using namespace k230::inspector;
using namespace k230::test;

TEST(PcmRing, ExtractsExactWindowAcrossChunks) {
  PcmRing ring(2'000'000);
  // 20 ms chunks at 48 kHz stereo, PTS 1.000s..1.100s, value = chunk index+1
  for (int i = 0; i < 5; ++i) ring.push(make_pcm(1'000'000 + i * 20'000, 20'000, static_cast<std::int16_t>(i + 1)));
  EXPECT_EQ(ring.oldest_pts_us(), 1'000'000);
  EXPECT_EQ(ring.newest_end_pts_us(), 1'100'000);

  std::int64_t filled = 0;
  auto s = ring.extract(1'010'000, 1'050'000, &filled);  // 40 ms
  EXPECT_EQ(filled, 40'000);
  ASSERT_EQ(s.size(), 40u * 48 * 2);
  EXPECT_EQ(s.front(), 1);  // second half of chunk 0
  EXPECT_EQ(s[(10 * 48 * 2)], 2);
  EXPECT_EQ(s[(30 * 48 * 2)], 3);
  EXPECT_EQ(s.back(), 3);
}

TEST(PcmRing, ZeroFillsGapsAndReportsFilled) {
  PcmRing ring(2'000'000);
  ring.push(make_pcm(1'000'000, 20'000, 7));
  ring.push(make_pcm(1'040'000, 20'000, 9));  // 20 ms hole between them

  std::int64_t filled = 0;
  auto s = ring.extract(990'000, 1'070'000, &filled);  // 80 ms window, 40 ms real
  ASSERT_EQ(s.size(), 80u * 48 * 2);
  EXPECT_EQ(filled, 40'000);
  EXPECT_EQ(s[0], 0);                // before first chunk
  EXPECT_EQ(s[15 * 48 * 2], 7);      // inside chunk 0
  EXPECT_EQ(s[30 * 48 * 2], 0);      // in the hole
  EXPECT_EQ(s[55 * 48 * 2], 9);      // inside chunk 1
  EXPECT_EQ(s.back(), 0);            // after last chunk
}

TEST(PcmRing, TrimsHistory) {
  PcmRing ring(100'000);
  for (int i = 0; i < 20; ++i) ring.push(make_pcm(i * 20'000, 20'000, 1));
  EXPECT_LE(ring.newest_end_pts_us() - ring.oldest_pts_us(), 100'000 + 20'000);
  ring.drop_before(350'000);
  EXPECT_GE(ring.oldest_pts_us(), 340'000);  // chunk containing 350 ms is kept
}

TEST(PcmRing, ExtractFromEmptyIsSilence) {
  PcmRing ring;
  std::int64_t filled = -1;
  auto s = ring.extract(0, 10'000, &filled);
  EXPECT_EQ(filled, 0);
  EXPECT_EQ(s.size(), 10u * 48 * 2);
}
