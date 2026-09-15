#include <cstring>

#include "k230/inspector/decoder.hpp"
#include "k230/log.hpp"

namespace k230::inspector {

bool RawPcmAudioDecoder::open(CodecId codec) { return codec == CodecId::Raw; }

bool RawPcmAudioDecoder::decode(const MediaPacket& packet, std::vector<PcmChunk>& out) {
  if (packet.is_config || packet.data.size() < 4) return true;
  PcmChunk chunk;
  chunk.pts_us = packet.pts_us;
  chunk.sample_rate = 48000;
  chunk.channels = 2;
  chunk.samples.resize(packet.data.size() / 2);
  // scrcpy sends native-endian PCM from the phone's AudioRecord, which is
  // little-endian on every Android device in existence.
  std::memcpy(chunk.samples.data(), packet.data.data(), chunk.samples.size() * 2);
  out.push_back(std::move(chunk));
  return true;
}

std::unique_ptr<AudioDecoder> make_default_audio_decoder(CodecId codec) {
  if (codec == CodecId::Raw) return std::make_unique<RawPcmAudioDecoder>();
#ifdef K230_HAS_FFMPEG
  return make_ffmpeg_audio_decoder();
#else
  K230_LOG_ERROR("decoder") << "no decoder for audio codec " << to_string(codec)
                            << " on this target; run scrcpy with audio_codec=raw";
  return nullptr;
#endif
}

std::unique_ptr<VideoDecoder> make_default_video_decoder() {
#ifdef K230_HAS_FFMPEG
  return make_ffmpeg_video_decoder();
#else
  return make_vdec_video_decoder();
#endif
}

}  // namespace k230::inspector
