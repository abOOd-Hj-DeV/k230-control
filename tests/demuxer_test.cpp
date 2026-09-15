#include "k230/scrcpy_demuxer.hpp"

#include <gtest/gtest.h>

#include "test_helpers.hpp"

using namespace k230;
using namespace k230::test;

namespace {

struct Capture {
  std::vector<MediaPacket> packets;
  std::vector<VideoSessionInfo> sessions;
  std::vector<CodecId> codecs;
  std::vector<std::string> names;

  ScrcpyDemuxer::Callbacks callbacks() {
    ScrcpyDemuxer::Callbacks cb;
    cb.on_packet = [this](MediaPacket&& p) { packets.push_back(std::move(p)); };
    cb.on_session = [this](const VideoSessionInfo& s) { sessions.push_back(s); };
    cb.on_codec = [this](CodecId c) { codecs.push_back(c); };
    cb.on_device_name = [this](const std::string& n) { names.push_back(n); };
    return cb;
  }
};

std::vector<std::uint8_t> video_stream() {
  std::vector<std::uint8_t> s;
  append_codec_id(s, "h264");
  append_video_session(s, 800, 1600, false);
  append_scrcpy_packet(s, ScrcpyDemuxer::kFlagConfig, {0x00, 0x00, 0x00, 0x01, 0x67, 0xAA});  // SPS
  append_scrcpy_packet(s, ScrcpyDemuxer::kFlagKeyFrame | 1'000'000u, {0x00, 0x00, 0x00, 0x01, 0x65, 0x01, 0x02});
  append_scrcpy_packet(s, 1'100'000u, {0x00, 0x00, 0x00, 0x01, 0x41, 0x03});
  append_video_session(s, 1600, 800, true);
  append_scrcpy_packet(s, ScrcpyDemuxer::kFlagKeyFrame | 1'200'000u, {0x00, 0x00, 0x00, 0x01, 0x65, 0x04});
  return s;
}

void check_video(const Capture& c) {
  ASSERT_EQ(c.codecs.size(), 1u);
  EXPECT_EQ(c.codecs[0], CodecId::H264);
  ASSERT_EQ(c.sessions.size(), 2u);
  EXPECT_EQ(c.sessions[0].width, 800u);
  EXPECT_EQ(c.sessions[0].height, 1600u);
  EXPECT_FALSE(c.sessions[0].client_resized);
  EXPECT_EQ(c.sessions[1].width, 1600u);
  EXPECT_EQ(c.sessions[1].height, 800u);
  EXPECT_TRUE(c.sessions[1].client_resized);

  ASSERT_EQ(c.packets.size(), 4u);
  EXPECT_TRUE(c.packets[0].is_config);
  EXPECT_EQ(c.packets[0].pts_us, kNoPts);
  EXPECT_EQ(c.packets[0].data.size(), 6u);

  EXPECT_FALSE(c.packets[1].is_config);
  EXPECT_TRUE(c.packets[1].is_key_frame);
  EXPECT_EQ(c.packets[1].pts_us, 1'000'000);
  EXPECT_EQ(c.packets[1].data.size(), 7u);
  EXPECT_EQ(c.packets[1].codec, CodecId::H264);
  EXPECT_EQ(c.packets[1].stream, StreamType::Video);

  EXPECT_FALSE(c.packets[2].is_key_frame);
  EXPECT_EQ(c.packets[2].pts_us, 1'100'000);

  EXPECT_TRUE(c.packets[3].is_key_frame);
  EXPECT_EQ(c.packets[3].pts_us, 1'200'000);
}

}  // namespace

TEST(ScrcpyDemuxer, ParsesVideoStreamInOneChunk) {
  Capture c;
  ScrcpyDemuxer d({StreamType::Video, false, false, true}, c.callbacks());
  ASSERT_TRUE(d.feed(video_stream()));
  EXPECT_FALSE(d.failed()) << d.error();
  check_video(c);
  EXPECT_EQ(d.stats().packets, 4u);
  EXPECT_EQ(d.stats().config_packets, 1u);
  EXPECT_EQ(d.stats().key_frames, 2u);
  EXPECT_EQ(d.stats().sessions, 2u);
}

TEST(ScrcpyDemuxer, ParsesVideoStreamByteByByte) {
  Capture c;
  ScrcpyDemuxer d({StreamType::Video, false, false, true}, c.callbacks());
  for (std::uint8_t b : video_stream()) ASSERT_TRUE(d.feed(&b, 1));
  EXPECT_FALSE(d.failed()) << d.error();
  check_video(c);
}

TEST(ScrcpyDemuxer, ParsesVideoStreamInOddChunks) {
  for (std::size_t chunk : {2u, 3u, 5u, 7u, 11u, 13u, 17u}) {
    Capture c;
    ScrcpyDemuxer d({StreamType::Video, false, false, true}, c.callbacks());
    const auto s = video_stream();
    for (std::size_t i = 0; i < s.size(); i += chunk) {
      ASSERT_TRUE(d.feed(s.data() + i, std::min(chunk, s.size() - i)));
    }
    EXPECT_FALSE(d.failed()) << d.error();
    check_video(c);
  }
}

TEST(ScrcpyDemuxer, HandlesDummyByteAndDeviceName) {
  std::vector<std::uint8_t> s;
  s.push_back(0x00);  // dummy byte
  std::string name = "Pixel 7";
  name.resize(ScrcpyDemuxer::kDeviceNameSize, '\0');
  s.insert(s.end(), name.begin(), name.end());
  append_codec_id(s, "h265");
  append_video_session(s, 720, 1280, false);
  append_scrcpy_packet(s, 42, {0x01});

  Capture c;
  ScrcpyDemuxer d({StreamType::Video, true, true, true}, c.callbacks());
  ASSERT_TRUE(d.feed(s));
  ASSERT_EQ(c.names.size(), 1u);
  EXPECT_EQ(c.names[0], "Pixel 7");
  ASSERT_EQ(c.codecs.size(), 1u);
  EXPECT_EQ(c.codecs[0], CodecId::H265);
  ASSERT_EQ(c.packets.size(), 1u);
  EXPECT_EQ(c.packets[0].pts_us, 42);
}

TEST(ScrcpyDemuxer, AudioStreamHasNoSessionPackets) {
  std::vector<std::uint8_t> s;
  append_codec_id(s, "\0raw");
  append_scrcpy_packet(s, 5'000'000u, std::vector<std::uint8_t>(3840, 0x11));
  append_scrcpy_packet(s, 5'020'000u, std::vector<std::uint8_t>(3840, 0x22));

  Capture c;
  ScrcpyDemuxer d({StreamType::Audio, false, false, true}, c.callbacks());
  ASSERT_TRUE(d.feed(s));
  ASSERT_EQ(c.codecs.size(), 1u);
  EXPECT_EQ(c.codecs[0], CodecId::Raw);
  EXPECT_TRUE(c.sessions.empty());
  ASSERT_EQ(c.packets.size(), 2u);
  EXPECT_EQ(c.packets[0].stream, StreamType::Audio);
  EXPECT_EQ(c.packets[0].codec, CodecId::Raw);
  EXPECT_EQ(c.packets[0].pts_us, 5'000'000);
  EXPECT_EQ(c.packets[1].pts_us, 5'020'000);
  EXPECT_EQ(c.packets[1].data.size(), 3840u);
}

TEST(ScrcpyDemuxer, OpusConfigPacketIsFlagged) {
  std::vector<std::uint8_t> s;
  append_codec_id(s, "opus");
  append_scrcpy_packet(s, ScrcpyDemuxer::kFlagConfig, {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'});
  append_scrcpy_packet(s, 100, {0xFC, 0xFF, 0xFE});
  Capture c;
  ScrcpyDemuxer d({StreamType::Audio, false, false, true}, c.callbacks());
  ASSERT_TRUE(d.feed(s));
  ASSERT_EQ(c.packets.size(), 2u);
  EXPECT_TRUE(c.packets[0].is_config);
  EXPECT_EQ(c.packets[1].pts_us, 100);
}

TEST(ScrcpyDemuxer, LargePtsIsPreservedWithoutFlagBits) {
  const std::uint64_t pts = (std::uint64_t{1} << 60) + 12345;
  std::vector<std::uint8_t> s;
  append_scrcpy_packet(s, ScrcpyDemuxer::kFlagKeyFrame | pts, {0x00});
  Capture c;
  ScrcpyDemuxer d({StreamType::Video, false, false, false}, c.callbacks());
  ASSERT_TRUE(d.feed(s));
  ASSERT_EQ(c.packets.size(), 1u);
  EXPECT_EQ(static_cast<std::uint64_t>(c.packets[0].pts_us), pts);
  EXPECT_TRUE(c.packets[0].is_key_frame);
}

TEST(ScrcpyDemuxer, RejectsOversizedPacket) {
  std::vector<std::uint8_t> s;
  std::uint8_t h[12];
  write64be(h, 0);
  write32be(h + 8, 1u << 20);
  s.insert(s.end(), h, h + 12);
  Capture c;
  ScrcpyDemuxer::Options o{StreamType::Video, false, false, false};
  o.max_packet_size = 1024;
  ScrcpyDemuxer d(o, c.callbacks());
  EXPECT_FALSE(d.feed(s));
  EXPECT_TRUE(d.failed());
  EXPECT_TRUE(c.packets.empty());
}

TEST(ScrcpyDemuxer, RejectsUnknownCodecId) {
  std::vector<std::uint8_t> s;
  append_codec_id(s, "zzzz");
  Capture c;
  ScrcpyDemuxer d({StreamType::Video, false, false, true}, c.callbacks());
  EXPECT_FALSE(d.feed(s));
  EXPECT_TRUE(d.failed());
}
