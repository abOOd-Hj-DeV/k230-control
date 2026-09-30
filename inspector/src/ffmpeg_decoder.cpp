// Software decoders for PC development. Not built for the K230 targets.

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "k230/inspector/decoder.hpp"
#include "k230/log.hpp"

namespace k230::inspector {

namespace {

constexpr const char* kTag = "ffmpeg";

AVCodecID to_av_codec(CodecId id) {
  switch (id) {
    case CodecId::H264: return AV_CODEC_ID_H264;
    case CodecId::H265: return AV_CODEC_ID_HEVC;
    case CodecId::AV1: return AV_CODEC_ID_AV1;
    case CodecId::Opus: return AV_CODEC_ID_OPUS;
    case CodecId::Aac: return AV_CODEC_ID_AAC;
    case CodecId::Flac: return AV_CODEC_ID_FLAC;
    default: return AV_CODEC_ID_NONE;
  }
}

struct AvContext {
  AVCodecContext* ctx = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;

  ~AvContext() {
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (ctx) avcodec_free_context(&ctx);
  }

  bool open(CodecId id) {
    const AVCodec* codec = avcodec_find_decoder(to_av_codec(id));
    if (!codec) {
      K230_LOG_ERROR(kTag) << "no libavcodec decoder for " << to_string(id);
      return false;
    }
    ctx = avcodec_alloc_context3(codec);
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!ctx || !pkt || !frame) return false;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->pkt_timebase = {1, 1'000'000};
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
      K230_LOG_ERROR(kTag) << "avcodec_open2 failed for " << to_string(id);
      return false;
    }
    return true;
  }
};

class FfmpegVideoDecoder final : public VideoDecoder {
 public:
  ~FfmpegVideoDecoder() override {
    if (sws_) sws_freeContext(sws_);
  }

  bool open(CodecId codec) override { return av_.open(codec); }
  const char* name() const override { return "ffmpeg"; }

  bool decode(const MediaPacket& packet, std::vector<VideoFrame>& out) override {
    if (packet.is_config) {
      // Annex-B SPS/PPS: keep it and prepend to the next access unit, exactly
      // like the official scrcpy client does (its decoder has no extradata).
      pending_config_ = packet.data;
      return true;
    }
    std::vector<std::uint8_t> merged;
    const std::uint8_t* data = packet.data.data();
    std::size_t size = packet.data.size();
    if (!pending_config_.empty()) {
      merged.reserve(pending_config_.size() + size);
      merged.insert(merged.end(), pending_config_.begin(), pending_config_.end());
      merged.insert(merged.end(), packet.data.begin(), packet.data.end());
      pending_config_.clear();
      data = merged.data();
      size = merged.size();
    }

    if (av_new_packet(av_.pkt, static_cast<int>(size)) < 0) return false;
    std::memcpy(av_.pkt->data, data, size);
    av_.pkt->pts = packet.pts_us;
    av_.pkt->dts = packet.pts_us;
    if (packet.is_key_frame) av_.pkt->flags |= AV_PKT_FLAG_KEY;

    int ret = avcodec_send_packet(av_.ctx, av_.pkt);
    av_packet_unref(av_.pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
      K230_LOG_WARN(kTag) << "avcodec_send_packet: " << ret;
      avcodec_flush_buffers(av_.ctx);
      return false;
    }
    return receive(out);
  }

  void flush(std::vector<VideoFrame>& out) override {
    avcodec_send_packet(av_.ctx, nullptr);
    receive(out);
  }

 private:
  bool receive(std::vector<VideoFrame>& out) {
    for (;;) {
      int ret = avcodec_receive_frame(av_.ctx, av_.frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
      if (ret < 0) {
        avcodec_flush_buffers(av_.ctx);
        out.clear();
        return false;
      }
      if (av_.frame->decode_error_flags != 0 || (av_.frame->flags & AV_FRAME_FLAG_CORRUPT) != 0) {
        av_frame_unref(av_.frame);
        avcodec_flush_buffers(av_.ctx);
        out.clear();
        return false;
      }
      out.push_back(convert(av_.frame));
      av_frame_unref(av_.frame);
    }
  }

  VideoFrame convert(AVFrame* f) {
    VideoFrame vf;
    vf.width = static_cast<std::uint32_t>(f->width);
    vf.height = static_cast<std::uint32_t>(f->height);
    vf.pts_us = f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
#if LIBAVUTIL_VERSION_MAJOR >= 58
    vf.key_frame = (f->flags & AV_FRAME_FLAG_KEY) != 0;
#else
    vf.key_frame = f->key_frame != 0;
#endif
    vf.format = PixelFormat::I420;
    vf.data.resize(static_cast<std::size_t>(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, f->width, f->height, 1)));

    std::uint8_t* dst[4] = {};
    int dst_linesize[4] = {};
    av_image_fill_arrays(dst, dst_linesize, vf.data.data(), AV_PIX_FMT_YUV420P, f->width, f->height, 1);

    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
      av_image_copy(dst, dst_linesize, const_cast<const std::uint8_t**>(f->data), f->linesize,
                    AV_PIX_FMT_YUV420P, f->width, f->height);
    } else {
      sws_ = sws_getCachedContext(sws_, f->width, f->height, static_cast<AVPixelFormat>(f->format),
                                  f->width, f->height, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
      sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dst_linesize);
    }
    return vf;
  }

  AvContext av_;
  SwsContext* sws_ = nullptr;
  std::vector<std::uint8_t> pending_config_;
};

class FfmpegAudioDecoder final : public AudioDecoder {
 public:
  bool open(CodecId codec) override {
    codec_ = codec;
    return av_.open(codec);
  }
  const char* name() const override { return "ffmpeg-audio"; }

  bool decode(const MediaPacket& packet, std::vector<PcmChunk>& out) override {
    if (packet.is_config) {
      // Opus/FLAC/AAC codec headers must be passed as extradata before the
      // first real packet; re-open the codec with them.
      AvContext fresh;
      const AVCodec* codec = avcodec_find_decoder(to_av_codec(codec_));
      if (!codec) return false;
      fresh.ctx = avcodec_alloc_context3(codec);
      fresh.pkt = av_packet_alloc();
      fresh.frame = av_frame_alloc();
      if (!fresh.ctx || !fresh.pkt || !fresh.frame) return false;
      fresh.ctx->pkt_timebase = {1, 1'000'000};
      fresh.ctx->extradata = static_cast<std::uint8_t*>(av_mallocz(packet.data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
      std::memcpy(fresh.ctx->extradata, packet.data.data(), packet.data.size());
      fresh.ctx->extradata_size = static_cast<int>(packet.data.size());
      if (avcodec_open2(fresh.ctx, codec, nullptr) < 0) return false;
      std::swap(av_.ctx, fresh.ctx);
      std::swap(av_.pkt, fresh.pkt);
      std::swap(av_.frame, fresh.frame);
      return true;
    }

    if (av_new_packet(av_.pkt, static_cast<int>(packet.data.size())) < 0) return false;
    std::memcpy(av_.pkt->data, packet.data.data(), packet.data.size());
    av_.pkt->pts = packet.pts_us;
    int ret = avcodec_send_packet(av_.ctx, av_.pkt);
    av_packet_unref(av_.pkt);
    if (ret < 0) {
      K230_LOG_WARN(kTag) << "audio avcodec_send_packet: " << ret;
      return true;
    }
    for (;;) {
      ret = avcodec_receive_frame(av_.ctx, av_.frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
      if (ret < 0) return false;
      out.push_back(to_pcm(av_.frame));
      av_frame_unref(av_.frame);
    }
  }

 private:
  static std::int16_t clamp16(float v) {
    return static_cast<std::int16_t>(std::max(-32768.0f, std::min(32767.0f, v * 32768.0f)));
  }

  static PcmChunk to_pcm(AVFrame* f) {
    PcmChunk c;
    c.pts_us = f->pts == AV_NOPTS_VALUE ? -1 : f->pts;
    c.sample_rate = static_cast<std::uint32_t>(f->sample_rate);
#if LIBAVUTIL_VERSION_MAJOR >= 58
    c.channels = static_cast<std::uint16_t>(f->ch_layout.nb_channels);
#else
    c.channels = static_cast<std::uint16_t>(f->channels);
#endif
    const int n = f->nb_samples;
    c.samples.resize(static_cast<std::size_t>(n) * c.channels);
    switch (f->format) {
      case AV_SAMPLE_FMT_S16:
        std::memcpy(c.samples.data(), f->data[0], c.samples.size() * 2);
        break;
      case AV_SAMPLE_FMT_S16P:
        for (int ch = 0; ch < c.channels; ++ch) {
          auto* src = reinterpret_cast<const std::int16_t*>(f->data[ch]);
          for (int i = 0; i < n; ++i) c.samples[static_cast<std::size_t>(i) * c.channels + ch] = src[i];
        }
        break;
      case AV_SAMPLE_FMT_FLT: {
        auto* src = reinterpret_cast<const float*>(f->data[0]);
        for (std::size_t i = 0; i < c.samples.size(); ++i) c.samples[i] = clamp16(src[i]);
        break;
      }
      case AV_SAMPLE_FMT_FLTP:
        for (int ch = 0; ch < c.channels; ++ch) {
          auto* src = reinterpret_cast<const float*>(f->data[ch]);
          for (int i = 0; i < n; ++i) c.samples[static_cast<std::size_t>(i) * c.channels + ch] = clamp16(src[i]);
        }
        break;
      default:
        K230_LOG_WARN(kTag) << "unsupported sample format " << f->format;
        c.samples.clear();
    }
    return c;
  }

  AvContext av_;
  CodecId codec_ = CodecId::Unknown;
};

}  // namespace

std::unique_ptr<VideoDecoder> make_ffmpeg_video_decoder() { return std::make_unique<FfmpegVideoDecoder>(); }
std::unique_ptr<AudioDecoder> make_ffmpeg_audio_decoder() { return std::make_unique<FfmpegAudioDecoder>(); }

}  // namespace k230::inspector
