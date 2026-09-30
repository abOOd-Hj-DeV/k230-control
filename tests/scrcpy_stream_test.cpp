// End-to-end on PC: a synthetic scrcpy byte stream (real H.264 from libx264 +
// raw PCM) goes through ScrcpyDemuxer -> InProcessQueue -> InspectorPipeline,
// exactly the path the two K230 cores will share, minus DATAFIFO.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include "k230/inspector/pipeline.hpp"
#include "k230/ipc/channel.hpp"
#include "k230/scrcpy_demuxer.hpp"
#include "test_helpers.hpp"

using namespace k230;
using namespace k230::inspector;
using namespace k230::test;

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr int kFps = 10;
constexpr int kFrames = 30;
constexpr std::int64_t kFrameUs = 1'000'000 / kFps;
constexpr std::int64_t kBasePts = 7'000'000;  // phone clock does not start at zero

struct Encoded {
  std::vector<std::uint8_t> config;
  std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> frames;  // pts_us, annex-b AU
};

// Frames 0..14: blue screen. Frames 15..29: skin tone (should trip the heuristic).
void paint(AVFrame* f, int index) {
  const bool skin = index >= kFrames / 2;
  const std::uint8_t y = skin ? 150 : 60;
  const std::uint8_t cb = skin ? 105 : 200;
  const std::uint8_t cr = skin ? 150 : 110;
  for (int r = 0; r < kHeight; ++r) std::memset(f->data[0] + r * f->linesize[0], y, kWidth);
  for (int r = 0; r < kHeight / 2; ++r) {
    std::memset(f->data[1] + r * f->linesize[1], cb, kWidth / 2);
    std::memset(f->data[2] + r * f->linesize[2], cr, kWidth / 2);
  }
}

bool encode(Encoded& out) {
  const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
  if (!codec) return false;
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  ctx->width = kWidth;
  ctx->height = kHeight;
  ctx->time_base = {1, 1'000'000};
  ctx->framerate = {kFps, 1};
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->gop_size = 10;
  ctx->max_b_frames = 0;
  ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // SPS/PPS in extradata, like scrcpy's config packet
  av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
  av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    return false;
  }
  out.config.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);

  AVFrame* frame = av_frame_alloc();
  frame->format = ctx->pix_fmt;
  frame->width = kWidth;
  frame->height = kHeight;
  av_frame_get_buffer(frame, 0);
  AVPacket* pkt = av_packet_alloc();

  auto drain = [&]() {
    while (avcodec_receive_packet(ctx, pkt) == 0) {
      out.frames.emplace_back(pkt->pts, std::vector<std::uint8_t>(pkt->data, pkt->data + pkt->size));
      av_packet_unref(pkt);
    }
  };
  for (int i = 0; i < kFrames; ++i) {
    av_frame_make_writable(frame);
    paint(frame, i);
    frame->pts = kBasePts + i * kFrameUs;
    avcodec_send_frame(ctx, frame);
    drain();
  }
  avcodec_send_frame(ctx, nullptr);
  drain();

  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&ctx);
  return out.frames.size() == kFrames;
}

std::vector<std::uint8_t> build_video_stream(const Encoded& e) {
  std::vector<std::uint8_t> s;
  s.push_back(0x00);  // dummy byte (send_dummy_byte=true)
  append_codec_id(s, "h264");
  append_video_session(s, kWidth, kHeight, false);
  append_scrcpy_packet(s, ScrcpyDemuxer::kFlagConfig, e.config);
  for (std::size_t i = 0; i < e.frames.size(); ++i) {
    std::uint64_t flags = static_cast<std::uint64_t>(e.frames[i].first);
    if (i % 10 == 0) flags |= ScrcpyDemuxer::kFlagKeyFrame;
    append_scrcpy_packet(s, flags, e.frames[i].second);
  }
  return s;
}

std::vector<std::uint8_t> build_audio_stream() {
  std::vector<std::uint8_t> s;
  s.push_back(0x00);
  append_codec_id(s, "\0raw");
  // 20 ms s16le stereo chunks, starting 0.6 s before the first frame and
  // ending 0.4 s after the last one.
  const std::int64_t start = kBasePts - 600'000;
  const std::int64_t end = kBasePts + kFrames * kFrameUs + 400'000;
  for (std::int64_t pts = start; pts < end; pts += 20'000) {
    std::vector<std::uint8_t> payload(960 * 2 * 2);
    // constant amplitude so RMS is deterministic
    for (std::size_t i = 0; i < payload.size(); i += 2) {
      payload[i] = 0x00;
      payload[i + 1] = 0x10;  // 0x1000 = 4096
    }
    append_scrcpy_packet(s, static_cast<std::uint64_t>(pts), payload);
  }
  return s;
}

class CollectSink final : public ipc::VerdictSink {
 public:
  bool push(Verdict&& v) override {
    verdicts.push_back(v);
    return true;
  }
  void close() override { closed = true; }
  std::vector<Verdict> verdicts;
  bool closed = false;
};

}  // namespace

TEST(EndToEnd, ScrcpyStreamThroughInspector) {
  Encoded enc;
  if (!encode(enc)) GTEST_SKIP() << "libx264 encoder not available in this FFmpeg build";

  auto queue = std::make_shared<ipc::InProcessQueue<MediaPacket>>(1024, ipc::keep_config_packets);

  // Little core: two demuxers (one per socket), fed in odd-sized chunks.
  std::vector<MediaPacket> vpk, apk;
  auto make_demuxer = [](StreamType type, std::vector<MediaPacket>& out) {
    ScrcpyDemuxer::Callbacks cb;
    cb.on_packet = [&out](MediaPacket&& p) { out.push_back(std::move(p)); };
    return ScrcpyDemuxer({type, true, false, true}, cb);
  };
  ScrcpyDemuxer video = make_demuxer(StreamType::Video, vpk);
  ScrcpyDemuxer audio = make_demuxer(StreamType::Audio, apk);
  const auto vs = build_video_stream(enc);
  const auto as = build_audio_stream();
  for (std::size_t i = 0; i < vs.size(); i += 700) ASSERT_TRUE(video.feed(vs.data() + i, std::min<std::size_t>(700, vs.size() - i))) << video.error();
  for (std::size_t i = 0; i < as.size(); i += 1500) ASSERT_TRUE(audio.feed(as.data() + i, std::min<std::size_t>(1500, as.size() - i))) << audio.error();
  EXPECT_EQ(video.stats().packets, static_cast<std::uint64_t>(kFrames + 1));
  EXPECT_EQ(video.stats().config_packets, 1u);
  EXPECT_EQ(video.stats().sessions, 1u);
  ASSERT_EQ(vpk.size(), static_cast<std::size_t>(kFrames + 1));

  // Transport: the two socket readers race in reality; here we merge by PTS
  // (what the phone emits in wall time) so the test is deterministic.
  std::size_t vi = 0, ai = 0;
  while (vi < vpk.size() || ai < apk.size()) {
    const bool take_video = vi < vpk.size() && (ai >= apk.size() || vpk[vi].is_config || vpk[vi].pts_us <= apk[ai].pts_us);
    queue->push(std::move(take_video ? vpk[vi++] : apk[ai++]));
  }
  queue->close();

  // Big core.
  PipelineConfig cfg;
  cfg.sync.audio_before_us = 500'000;
  cfg.sync.audio_after_us = 200'000;
  cfg.policy.confirm_frames = 3;
  cfg.policy.warn_threshold = 0.5f;
  cfg.policy.block_threshold = 0.9f;
  cfg.policy.cooldown_us = 100'000'000;  // one block only
  auto sink = std::make_shared<CollectSink>();
  std::vector<SyncedSample> samples;
  InspectorPipeline pipeline(cfg, queue, sink, make_default_video_decoder(), std::make_unique<HeuristicAnalyzer>());
  pipeline.set_observer([&](const SyncedSample& s, const Scores&, const Verdict&) { samples.push_back(s); });
  pipeline.run();

  EXPECT_TRUE(sink->closed);
  EXPECT_EQ(pipeline.stats().frames_decoded, static_cast<std::uint64_t>(kFrames));
  ASSERT_EQ(samples.size(), static_cast<std::size_t>(kFrames));
  ASSERT_EQ(sink->verdicts.size(), static_cast<std::size_t>(kFrames));

  for (int i = 0; i < kFrames; ++i) {
    const auto& s = samples[static_cast<std::size_t>(i)];
    EXPECT_EQ(s.frame.pts_us, kBasePts + i * kFrameUs) << "phone PTS must survive the whole pipeline";
    EXPECT_EQ(s.frame.width, static_cast<std::uint32_t>(kWidth));
    EXPECT_TRUE(s.audio_complete) << "frame " << i;
    EXPECT_EQ(s.audio_end_us - s.audio_start_us, 700'000);
    EXPECT_EQ(s.audio.size(), 700u * 48 * 2);
    EXPECT_EQ(s.audio[0], 0x1000);
    EXPECT_EQ(sink->verdicts[static_cast<std::size_t>(i)].pts_us, s.frame.pts_us);
  }

  // Blue half is safe, skin half escalates exactly once (debounce + cooldown).
  int blocks = 0;
  for (int i = 0; i < kFrames; ++i) {
    const auto& v = sink->verdicts[static_cast<std::size_t>(i)];
    if (i < kFrames / 2) {
      EXPECT_EQ(v.action, Action::Log) << i;
    }
    if (v.action == Action::Block) {
      ++blocks;
      EXPECT_EQ(v.category, Category::Nudity);
      EXPECT_GE(i, kFrames / 2 + 2);
    }
  }
  EXPECT_EQ(blocks, 1);
  EXPECT_EQ(pipeline.sync().stats().emitted_incomplete, 0u);
}

// A lost P-frame must not produce garbage frames: video is skipped until the
// next key frame (GOP is 10 in the encoder above).
TEST(EndToEnd, LostPacketSkipsToNextKeyFrame) {
  Encoded enc;
  if (!encode(enc)) GTEST_SKIP() << "libx264 encoder not available in this FFmpeg build";

  std::vector<MediaPacket> vpk;
  ScrcpyDemuxer::Callbacks cb;
  cb.on_packet = [&vpk](MediaPacket&& p) { vpk.push_back(std::move(p)); };
  ScrcpyDemuxer video({StreamType::Video, true, false, true}, cb);
  ASSERT_TRUE(video.feed(build_video_stream(enc))) << video.error();
  ASSERT_EQ(vpk.size(), static_cast<std::size_t>(kFrames + 1));  // config + frames

  auto queue = std::make_shared<ipc::InProcessQueue<MediaPacket>>(1024, ipc::keep_config_packets);
  constexpr std::size_t kLostFrame = 3;
  for (std::size_t i = 0; i < vpk.size(); ++i) {
    if (i == kLostFrame + 1) continue;  // index 0 is the config packet
    queue->push(std::move(vpk[i]));
  }
  queue->close();

  PipelineConfig cfg;
  cfg.sync.audio_enabled = false;
  auto sink = std::make_shared<CollectSink>();
  std::vector<std::int64_t> pts;
  InspectorPipeline pipeline(cfg, queue, sink, make_default_video_decoder(), std::make_unique<HeuristicAnalyzer>());
  pipeline.set_observer([&](const SyncedSample& s, const Scores&, const Verdict&) { pts.push_back(s.frame.pts_us); });
  pipeline.run();

  EXPECT_EQ(pipeline.stats().video_gaps, 1u);
  EXPECT_EQ(pipeline.stats().video_skipped, 6u);  // frames 4..9
  ASSERT_EQ(pts.size(), static_cast<std::size_t>(kFrames - 7));
  EXPECT_EQ(pts[2], kBasePts + 2 * kFrameUs);
  EXPECT_EQ(pts[3], kBasePts + 10 * kFrameUs) << "resumes at the next key frame";
}
