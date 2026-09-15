#include "k230/ipc/channel.hpp"

#include <gtest/gtest.h>

#include <thread>

using namespace k230;
using namespace std::chrono_literals;

namespace {
MediaPacket pkt(std::int64_t pts, bool config = false) {
  MediaPacket p;
  p.pts_us = pts;
  p.is_config = config;
  return p;
}
}  // namespace

TEST(InProcessQueue, FifoOrder) {
  ipc::InProcessQueue<MediaPacket> q(4);
  for (int i = 0; i < 3; ++i) EXPECT_TRUE(q.push(pkt(i)));
  for (int i = 0; i < 3; ++i) {
    auto p = q.pop(0ms);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->pts_us, i);
  }
  EXPECT_FALSE(q.pop(0ms));
}

TEST(InProcessQueue, DropsOldestNonConfigWhenFull) {
  ipc::InProcessQueue<MediaPacket> q(3, ipc::keep_config_packets);
  EXPECT_TRUE(q.push(pkt(-1, true)));  // SPS/PPS
  EXPECT_TRUE(q.push(pkt(1)));
  EXPECT_TRUE(q.push(pkt(2)));
  EXPECT_FALSE(q.push(pkt(3)));  // full: evicts pts=1, keeps config
  EXPECT_EQ(q.drops(), 1u);
  EXPECT_EQ(q.size(), 3u);

  auto a = q.pop(0ms);
  ASSERT_TRUE(a);
  EXPECT_TRUE(a->is_config);
  auto b = q.pop(0ms);
  ASSERT_TRUE(b);
  EXPECT_EQ(b->pts_us, 2);
  auto c = q.pop(0ms);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->pts_us, 3);
}

TEST(InProcessQueue, CloseWakesConsumer) {
  ipc::InProcessQueue<MediaPacket> q(4);
  std::thread t([&] {
    std::this_thread::sleep_for(20ms);
    q.close();
  });
  auto p = q.pop(5s);
  EXPECT_FALSE(p);
  EXPECT_TRUE(q.closed());
  EXPECT_FALSE(q.push(pkt(1)));
  t.join();
}
