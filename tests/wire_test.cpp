#include "k230/ipc/wire.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>

#include "k230/recording.hpp"

using namespace k230;

TEST(Wire, MediaPacketRoundTrip) {
  MediaPacket p;
  p.stream = StreamType::Audio;
  p.codec = CodecId::Opus;
  p.pts_us = 123'456'789'012LL;
  p.is_config = false;
  p.is_key_frame = true;
  p.data = {1, 2, 3, 4, 5};

  auto bytes = ipc::encode(p);
  ASSERT_EQ(bytes.size(), ipc::kPacketHeaderSize + 5);

  MediaPacket q;
  auto len = ipc::decode_header(bytes.data(), q);
  ASSERT_TRUE(len.has_value());
  EXPECT_EQ(*len, 5u);
  EXPECT_EQ(q.stream, StreamType::Audio);
  EXPECT_EQ(q.codec, CodecId::Opus);
  EXPECT_EQ(q.pts_us, 123'456'789'012LL);
  EXPECT_FALSE(q.is_config);
  EXPECT_TRUE(q.is_key_frame);
}

TEST(Wire, ConfigPacketKeepsNoPts) {
  MediaPacket p;
  p.codec = CodecId::H264;
  p.is_config = true;
  p.pts_us = kNoPts;
  auto bytes = ipc::encode(p);
  MediaPacket q;
  ASSERT_TRUE(ipc::decode_header(bytes.data(), q));
  EXPECT_TRUE(q.is_config);
  EXPECT_EQ(q.pts_us, kNoPts);
}

TEST(Wire, RejectsBadMagic) {
  std::uint8_t buf[ipc::kPacketHeaderSize] = {};
  MediaPacket q;
  EXPECT_FALSE(ipc::decode_header(buf, q).has_value());
}

TEST(Wire, VerdictRoundTrip) {
  Verdict v;
  v.pts_us = 987'654'321;
  v.action = Action::Block;
  v.category = Category::Nudity;
  v.confidence = 0.912345f;
  v.sequence = 77;
  std::uint8_t buf[ipc::kVerdictSize];
  ipc::encode(v, buf);
  auto w = ipc::decode_verdict(buf);
  ASSERT_TRUE(w.has_value());
  EXPECT_EQ(w->pts_us, v.pts_us);
  EXPECT_EQ(w->action, Action::Block);
  EXPECT_EQ(w->category, Category::Nudity);
  EXPECT_NEAR(w->confidence, v.confidence, 1e-5f);
  EXPECT_EQ(w->sequence, 77u);
}

TEST(Verdict, JsonLine) {
  Verdict v;
  v.pts_us = 5;
  v.action = Action::Warn;
  v.category = Category::Violence;
  v.confidence = 0.5f;
  v.sequence = 3;
  const std::string line = to_json_line(v);
  EXPECT_NE(line.find("\"action\":\"warn\""), std::string::npos);
  EXPECT_NE(line.find("\"category\":\"violence\""), std::string::npos);
  EXPECT_NE(line.find("\"pts_us\":5"), std::string::npos);
  EXPECT_EQ(line.back(), '\n');
}

TEST(Recording, WriteThenRead) {
  const auto path = std::filesystem::temp_directory_path() / "k230_test.k230rec";
  {
    RecordingWriter w(path.string());
    ASSERT_TRUE(w.ok());
    for (int i = 0; i < 3; ++i) {
      MediaPacket p;
      p.stream = i == 1 ? StreamType::Audio : StreamType::Video;
      p.codec = i == 1 ? CodecId::Raw : CodecId::H264;
      p.pts_us = 1000 * i;
      p.is_key_frame = i == 0;
      p.data.assign(static_cast<std::size_t>(10 + i), static_cast<std::uint8_t>(i));
      ASSERT_TRUE(w.write(p));
    }
  }
  RecordingReader r(path.string());
  ASSERT_TRUE(r.ok());
  for (int i = 0; i < 3; ++i) {
    auto p = r.next();
    ASSERT_TRUE(p.has_value()) << r.error();
    EXPECT_EQ(p->pts_us, 1000 * i);
    EXPECT_EQ(p->data.size(), static_cast<std::size_t>(10 + i));
    EXPECT_EQ(p->stream, i == 1 ? StreamType::Audio : StreamType::Video);
  }
  EXPECT_FALSE(r.next().has_value());
  EXPECT_TRUE(r.error().empty());
  std::filesystem::remove(path);
}
