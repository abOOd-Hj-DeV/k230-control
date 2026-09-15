#pragma once

#include <memory>
#include <string>
#include <vector>

#include "k230/inspector/frame.hpp"
#include "k230/media_packet.hpp"

namespace k230::inspector {

// Compressed video access units -> decoded frames.
//
// Implementations:
//   FfmpegVideoDecoder  software, PC only (libavcodec)
//   VdecVideoDecoder    K230 hardware decoder through the MPP VDEC API (big core)
//
// Config packets (SPS/PPS) arrive as separate MediaPackets with is_config set
// and no PTS; both implementations accept them through the same decode() call.
class VideoDecoder {
 public:
  virtual ~VideoDecoder() = default;
  virtual bool open(CodecId codec) = 0;
  // Appends zero or more decoded frames to `out`. Returns false on a fatal error.
  virtual bool decode(const MediaPacket& packet, std::vector<VideoFrame>& out) = 0;
  virtual void flush(std::vector<VideoFrame>& out) = 0;
  virtual const char* name() const = 0;
};

class AudioDecoder {
 public:
  virtual ~AudioDecoder() = default;
  virtual bool open(CodecId codec) = 0;
  virtual bool decode(const MediaPacket& packet, std::vector<PcmChunk>& out) = 0;
  virtual const char* name() const = 0;
};

// scrcpy "raw" audio: PCM s16le, 48 kHz, stereo. Nothing to decode, which is
// exactly why it is the recommended audio codec for the K230.
class RawPcmAudioDecoder final : public AudioDecoder {
 public:
  bool open(CodecId codec) override;
  bool decode(const MediaPacket& packet, std::vector<PcmChunk>& out) override;
  const char* name() const override { return "raw-pcm"; }
};

#ifdef K230_HAS_FFMPEG
std::unique_ptr<VideoDecoder> make_ffmpeg_video_decoder();
std::unique_ptr<AudioDecoder> make_ffmpeg_audio_decoder();
#endif

// Always declared; on a PC build open() fails with a clear message.
std::unique_ptr<VideoDecoder> make_vdec_video_decoder();

// Picks the right decoders for the build target.
std::unique_ptr<VideoDecoder> make_default_video_decoder();
std::unique_ptr<AudioDecoder> make_default_audio_decoder(CodecId codec);

}  // namespace k230::inspector
