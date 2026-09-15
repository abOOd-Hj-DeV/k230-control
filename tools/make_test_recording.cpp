// Generates a synthetic .k230rec (H.264 via libx264 + raw PCM) so the
// inspector can be exercised on a PC without a phone:
//
//   k230-make-test-recording out.k230rec [seconds=10] [fps=10]
//   k230-inspector --replay out.k230rec --realtime --verbose
//
// The picture alternates every 2 s between a "safe" blue screen and a
// skin-toned screen so the heuristic analyzer produces both Log and Block.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "k230/recording.hpp"

using namespace k230;

namespace {

constexpr int kW = 320;
constexpr int kH = 640;
constexpr std::int64_t kBasePts = 12'000'000;

void paint(AVFrame* f, double t) {
  const bool skin = static_cast<int>(t / 2.0) % 2 == 1;
  const std::uint8_t y = skin ? 150 : 60;
  const std::uint8_t cb = skin ? 105 : 200;
  const std::uint8_t cr = skin ? 150 : 110;
  for (int r = 0; r < kH; ++r) std::memset(f->data[0] + r * f->linesize[0], y, kW);
  for (int r = 0; r < kH / 2; ++r) {
    std::memset(f->data[1] + r * f->linesize[1], cb, kW / 2);
    std::memset(f->data[2] + r * f->linesize[2], cr, kW / 2);
  }
  // moving bar so the encoder has something to do
  const int x = static_cast<int>(std::fmod(t * 60.0, kW));
  for (int r = 0; r < kH; ++r) std::memset(f->data[0] + r * f->linesize[0] + x, 235, std::min(8, kW - x));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s out.k230rec [seconds] [fps]\n", argv[0]);
    return 2;
  }
  const std::string out_path = argv[1];
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 10;
  const int fps = argc > 3 ? std::atoi(argv[3]) : 10;
  const std::int64_t frame_us = 1'000'000 / fps;

  const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
  if (!codec) {
    std::fprintf(stderr, "libx264 encoder not available in this FFmpeg build\n");
    return 1;
  }
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  ctx->width = kW;
  ctx->height = kH;
  ctx->time_base = {1, 1'000'000};
  ctx->framerate = {fps, 1};
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->gop_size = fps * 2;
  ctx->max_b_frames = 0;
  ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
  av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
  if (avcodec_open2(ctx, codec, nullptr) < 0) return 1;

  RecordingWriter rec(out_path);
  if (!rec.ok()) {
    std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
    return 1;
  }

  MediaPacket cfg;
  cfg.stream = StreamType::Video;
  cfg.codec = CodecId::H264;
  cfg.is_config = true;
  cfg.data.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
  rec.write(cfg);

  AVFrame* frame = av_frame_alloc();
  frame->format = ctx->pix_fmt;
  frame->width = kW;
  frame->height = kH;
  av_frame_get_buffer(frame, 0);
  AVPacket* pkt = av_packet_alloc();

  std::int64_t audio_pts = kBasePts - 200'000;  // audio typically starts slightly before video
  std::uint64_t written = 0;
  const int total = seconds * fps;
  for (int i = 0; i <= total; ++i) {
    const std::int64_t vpts = kBasePts + i * frame_us;
    // interleave 20 ms PCM chunks (48 kHz stereo, 440 Hz tone) up to this frame
    while (audio_pts <= vpts) {
      MediaPacket a;
      a.stream = StreamType::Audio;
      a.codec = CodecId::Raw;
      a.pts_us = audio_pts;
      a.data.resize(960 * 2 * 2);
      for (int n = 0; n < 960; ++n) {
        const double t = static_cast<double>(audio_pts) / 1e6 + n / 48000.0;
        const auto v = static_cast<std::int16_t>(6000.0 * std::sin(2.0 * M_PI * 440.0 * t));
        for (int ch = 0; ch < 2; ++ch) {
          a.data[(n * 2 + ch) * 2] = static_cast<std::uint8_t>(v & 0xff);
          a.data[(n * 2 + ch) * 2 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
        }
      }
      rec.write(a);
      audio_pts += 20'000;
    }
    if (i == total) break;

    av_frame_make_writable(frame);
    paint(frame, static_cast<double>(i) / fps);
    frame->pts = vpts;
    avcodec_send_frame(ctx, frame);
    while (avcodec_receive_packet(ctx, pkt) == 0) {
      MediaPacket v;
      v.stream = StreamType::Video;
      v.codec = CodecId::H264;
      v.pts_us = pkt->pts;
      v.is_key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
      v.data.assign(pkt->data, pkt->data + pkt->size);
      rec.write(v);
      ++written;
      av_packet_unref(pkt);
    }
  }

  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&ctx);
  std::printf("wrote %llu video frames + audio to %s\n", static_cast<unsigned long long>(written), out_path.c_str());
  return 0;
}
