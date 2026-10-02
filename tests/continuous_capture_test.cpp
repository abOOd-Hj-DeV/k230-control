#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "k230/bridge/scrcpy_session.hpp"
#include "k230/inspector/frame_dump.hpp"

using namespace k230;
using namespace k230::inspector;

namespace {

VideoFrame frame_at(std::int64_t pts_us) {
  VideoFrame frame;
  frame.pts_us = pts_us;
  frame.width = 16;
  frame.height = 16;
  frame.data.assign(16 * 16 * 3 / 2, 128);
  return frame;
}

PcmChunk audio_at(std::int64_t pts_us, std::int16_t value) {
  PcmChunk chunk;
  chunk.pts_us = pts_us;
  chunk.sample_rate = 48000;
  chunk.channels = 2;
  chunk.samples.assign(960 * 2, value);
  return chunk;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{file}, {}};
}

}  // namespace

TEST(ScrcpyCommand, DuplicatesPlaybackAudioAndRequestsTenFps) {
  auto queue = std::make_shared<ipc::InProcessQueue<MediaPacket>>(8);
  bridge::ScrcpySession session(bridge::ScrcpyConfig{}, bridge::AdbController{}, queue);
  const auto command = session.server_command();
  for (const std::string option : {"audio=true", "audio_source=playback", "audio_dup=true",
                                   "audio_codec=raw", "max_fps=10", "send_frame_meta=true"}) {
    EXPECT_NE(std::find(command.begin(), command.end(), option), command.end()) << option;
  }
}

TEST(ContinuousCapture, KeepsTenFpsAndContiguousAudioBeyondEightSeconds) {
  const auto directory = std::filesystem::temp_directory_path() / "k230_continuous_capture";
  std::filesystem::create_directories(directory);
  ContinuousCapture capture(directory.string());
  constexpr std::int64_t base = 12'000'000;
  capture.push_audio(audio_at(base - 20'000, -1));
  for (int i = 0; i < 617; ++i) {
    if (i % 5 == 0 && i < 600) capture.push_frame(frame_at(base + i * 20'000));
    capture.push_audio(audio_at(base + i * 20'000, static_cast<std::int16_t>(i)));
  }
  EXPECT_TRUE(capture.finish());
  EXPECT_EQ(capture.images_written(), 120u);
  EXPECT_EQ(capture.audio_filled_us(), 12'340'000);
  EXPECT_EQ(capture.audio_duration_us(), 12'340'000);
  for (int i = 0; i < 120; ++i) {
    const auto path = directory / ("frame_" + std::to_string(i) + "_pts_" +
                                   std::to_string(base + i * 100'000) + ".png");
    EXPECT_TRUE(std::filesystem::exists(path)) << path;
  }
  for (int second = 0; second < 13; ++second) {
    const auto start = base + second * 1'000'000;
    const auto duration = second < 12 ? 1'000'000 : 340'000;
    const auto path = directory / ("audio_pts_" + std::to_string(start) + "_end_" +
                                   std::to_string(start + duration) + ".wav");
    const auto bytes = read_bytes(path);
    ASSERT_EQ(bytes.size(), 44u + duration * 48000 / 1'000'000 * 4) << path;
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "RIFF");
    for (std::size_t sample = 0; sample < (bytes.size() - 44) / 2; ++sample) {
      const auto expected = static_cast<std::uint16_t>(second * 50 + sample / (960 * 2));
      EXPECT_EQ(bytes[44 + sample * 2], expected & 0xff);
      EXPECT_EQ(bytes[45 + sample * 2], expected >> 8);
    }
  }
  std::filesystem::remove_all(directory);
}

TEST(ContinuousCapture, UsesPtsSlotsWithoutInventingMissingFrames) {
  const auto directory = std::filesystem::temp_directory_path() / "k230_continuous_video";
  std::filesystem::create_directories(directory);
  ContinuousCapture capture(directory.string(), false);
  constexpr std::int64_t base = 12'000'000;
  for (const auto offset : {0, 90'000, 105'000, 199'000, 320'000, 8'105'000}) {
    capture.push_frame(frame_at(base + offset));
  }
  EXPECT_TRUE(capture.finish());
  EXPECT_EQ(capture.images_written(), 4u);
  EXPECT_TRUE(std::filesystem::exists(directory / "frame_1_pts_12105000.png"));
  EXPECT_TRUE(std::filesystem::exists(directory / "frame_81_pts_20105000.png"));
  EXPECT_FALSE(std::filesystem::exists(directory / "frame_2_pts_12200000.png"));
  EXPECT_EQ(capture.audio_duration_us(), 0);
  std::filesystem::remove_all(directory);
}

TEST(ContinuousCapture, ZeroFillsAudioGapsAndReportsIncompleteCapture) {
  const auto directory = std::filesystem::temp_directory_path() / "k230_continuous_gap";
  std::filesystem::create_directories(directory);
  ContinuousCapture capture(directory.string());
  constexpr std::int64_t base = 12'000'000;
  capture.push_frame(frame_at(base));
  for (int i = 0; i < 50; ++i) {
    if (i != 20 && i != 21) capture.push_audio(audio_at(base + i * 20'000, -1234));
  }
  EXPECT_FALSE(capture.finish());
  EXPECT_EQ(capture.audio_duration_us(), 1'000'000);
  EXPECT_EQ(capture.audio_filled_us(), 960'000);
  const auto bytes = read_bytes(directory / "audio_pts_12000000_end_13000000.wav");
  ASSERT_EQ(bytes.size(), 44u + 48000u * 4u);
  EXPECT_EQ(bytes[44], 0x2e);
  EXPECT_EQ(bytes[45], 0xfb);
  for (std::size_t i = 44 + 19200 * 4; i < 44 + 21120 * 4; ++i) EXPECT_EQ(bytes[i], 0);
  EXPECT_EQ(bytes[44 + 21120 * 4], 0x2e);
  std::filesystem::remove_all(directory);
}
