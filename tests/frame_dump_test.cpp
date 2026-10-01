#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "k230/inspector/frame_dump.hpp"
#include "k230/inspector/decoder.hpp"
#include "test_helpers.hpp"

using namespace k230::inspector;
using namespace k230::test;

TEST(AudioDecode, RawPcmPreservesSignedLittleEndianSamples) {
  RawPcmAudioDecoder decoder;
  ASSERT_TRUE(decoder.open(k230::CodecId::Raw));
  k230::MediaPacket packet;
  packet.codec = k230::CodecId::Raw;
  packet.stream = k230::StreamType::Audio;
  packet.pts_us = 12'345'678;
  packet.data = {0x00, 0x80, 0xff, 0x7f, 0x34, 0x12, 0xcc, 0xed};
  std::vector<PcmChunk> chunks;
  ASSERT_TRUE(decoder.decode(packet, chunks));
  ASSERT_EQ(chunks.size(), 1u);
  EXPECT_EQ(chunks[0].pts_us, packet.pts_us);
  EXPECT_EQ(chunks[0].samples, (std::vector<std::int16_t>{-32768, 32767, 4660, -4660}));
  packet.data.pop_back();
  EXPECT_FALSE(decoder.decode(packet, chunks));
  EXPECT_EQ(chunks.size(), 1u);
}

TEST(FrameDump, PngIsDecodableAndPreservesPixels) {
  const auto path = std::filesystem::temp_directory_path() / "k230_frame_dump_test.png";
  auto frame = make_frame(1'000'000, 16, 16, 128, 128, 128);
  ASSERT_TRUE(write_png(frame, path.string()));
  std::ifstream file(path, std::ios::binary);
  std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{file}, {});
  ASSERT_GT(bytes.size(), 50u);
  EXPECT_EQ(std::string(bytes.begin() + 1, bytes.begin() + 4), "PNG");

  const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_PNG);
  ASSERT_NE(decoder, nullptr);
  AVCodecContext* context = avcodec_alloc_context3(decoder);
  AVPacket* packet = av_packet_alloc();
  AVFrame* decoded = av_frame_alloc();
  ASSERT_EQ(avcodec_open2(context, decoder, nullptr), 0);
  ASSERT_EQ(av_new_packet(packet, static_cast<int>(bytes.size())), 0);
  std::copy(bytes.begin(), bytes.end(), packet->data);
  EXPECT_EQ(avcodec_send_packet(context, packet), 0);
  EXPECT_EQ(avcodec_receive_frame(context, decoded), 0);
  EXPECT_EQ(decoded->width, 16);
  EXPECT_EQ(decoded->height, 16);
  EXPECT_NEAR(decoded->data[0][0], 130, 3);
  EXPECT_NEAR(decoded->data[0][1], 130, 3);
  EXPECT_NEAR(decoded->data[0][2], 130, 3);
  av_frame_free(&decoded);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  std::filesystem::remove(path);
}

TEST(FrameDump, EightSecondWavAndOneFramePerSecondSharePhonePts) {
  const auto directory = std::filesystem::temp_directory_path() / "k230_test_capture";
  std::filesystem::create_directories(directory);
  TestCapture capture(directory.string());
  constexpr std::int64_t base = 12'000'000;
  for (int i = 0; i < 400; ++i) {
    capture.push_audio(make_pcm(base + i * 20'000, 20'000, static_cast<std::int16_t>(i)));
  }
  for (int i = 0; i < 80; ++i) {
    capture.push_frame(make_frame(base + i * 100'000));
  }
  EXPECT_TRUE(capture.finish());
  EXPECT_EQ(capture.images_written(), 8u);
  EXPECT_EQ(capture.audio_filled_us(), 8'000'000);
  for (int i = 0; i < 8; ++i) {
    const auto path = directory / ("second_" + std::to_string(i) + "_pts_" +
                                   std::to_string(base + i * 1'000'000) + ".png");
    EXPECT_TRUE(std::filesystem::exists(path)) << path;
  }
  const auto wav = directory / "audio_pts_12000000_8s.wav";
  ASSERT_EQ(std::filesystem::file_size(wav), 44u + 8u * 48000u * 2u * 2u);
  std::ifstream file(wav, std::ios::binary);
  std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{file}, {});
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "RIFF");
  EXPECT_EQ(bytes[44], 0);
  EXPECT_EQ(bytes[45], 0);
  EXPECT_EQ(bytes[44 + 960 * 2 * 2], 1);
  EXPECT_EQ(bytes[44 + 960 * 2 * 2 + 1], 0);
  std::filesystem::remove_all(directory);
}

TEST(FrameDump, MissingSecondMarksCaptureIncomplete) {
  const auto directory = std::filesystem::temp_directory_path() / "k230_test_capture_gap";
  std::filesystem::create_directories(directory);
  TestCapture capture(directory.string());
  constexpr std::int64_t base = 12'000'000;
  for (int i = 0; i < 400; ++i) {
    capture.push_audio(make_pcm(base + i * 20'000, 20'000, 0));
  }
  for (int i = 0; i < 8; ++i) {
    if (i != 3) capture.push_frame(make_frame(base + i * 1'000'000));
  }
  EXPECT_FALSE(capture.finish());
  EXPECT_EQ(capture.images_written(), 7u);
  EXPECT_EQ(capture.audio_filled_us(), 8'000'000);
  std::filesystem::remove_all(directory);
}
