#include "k230/scrcpy_demuxer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "k230/byteorder.hpp"

namespace k230 {

const char* to_string(CodecId id) {
  switch (id) {
    case CodecId::H264: return "h264";
    case CodecId::H265: return "h265";
    case CodecId::AV1: return "av1";
    case CodecId::Opus: return "opus";
    case CodecId::Aac: return "aac";
    case CodecId::Flac: return "flac";
    case CodecId::Raw: return "raw";
    case CodecId::Unknown: break;
  }
  return "unknown";
}

namespace {
bool is_known(CodecId id) { return std::strcmp(to_string(id), "unknown") != 0; }
}  // namespace

const char* to_string(StreamType type) {
  return type == StreamType::Video ? "video" : "audio";
}

ScrcpyDemuxer::ScrcpyDemuxer(Options options, Callbacks callbacks)
    : options_(options), callbacks_(std::move(callbacks)) {
  if (options_.expect_dummy_byte) {
    state_ = State::DummyByte;
  } else if (options_.expect_device_name) {
    state_ = State::DeviceName;
  } else if (options_.expect_codec_id) {
    state_ = State::CodecId;
  } else {
    state_ = State::Header;
  }
}

bool ScrcpyDemuxer::feed(const std::uint8_t* data, std::size_t len) {
  if (failed_) return false;
  if (len == 0) return true;
  buffer_.insert(buffer_.end(), data, data + len);
  stats_.bytes += len;
  while (!failed_ && step()) {
  }
  compact();
  return !failed_;
}

void ScrcpyDemuxer::fail(std::string why) {
  failed_ = true;
  error_ = std::move(why);
  state_ = State::Failed;
}

void ScrcpyDemuxer::consume(std::size_t n) { read_pos_ += n; }

void ScrcpyDemuxer::compact() {
  if (read_pos_ == 0) return;
  if (read_pos_ == buffer_.size()) {
    buffer_.clear();
    read_pos_ = 0;
    return;
  }
  if (read_pos_ >= buffer_.size() / 2) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_pos_));
    read_pos_ = 0;
  }
}

bool ScrcpyDemuxer::step() {
  switch (state_) {
    case State::DummyByte: {
      if (available() < 1) return false;
      consume(1);
      state_ = options_.expect_device_name ? State::DeviceName
               : options_.expect_codec_id  ? State::CodecId
                                           : State::Header;
      return true;
    }

    case State::DeviceName: {
      if (available() < kDeviceNameSize) return false;
      const char* begin = reinterpret_cast<const char*>(cursor());
      std::size_t n = ::strnlen(begin, kDeviceNameSize);
      if (callbacks_.on_device_name) callbacks_.on_device_name(std::string(begin, n));
      consume(kDeviceNameSize);
      state_ = options_.expect_codec_id ? State::CodecId : State::Header;
      return true;
    }

    case State::CodecId: {
      if (available() < 4) return false;
      codec_ = static_cast<CodecId>(read32be(cursor()));
      if (!is_known(codec_)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "unknown codec id 0x%08x", static_cast<std::uint32_t>(codec_));
        fail(buf);
        return false;
      }
      consume(4);
      if (callbacks_.on_codec) callbacks_.on_codec(codec_);
      state_ = State::Header;
      return true;
    }

    case State::Header: {
      if (available() < kHeaderSize) return false;
      const std::uint8_t* h = cursor();

      if (options_.stream == StreamType::Video && (h[0] & 0x80u)) {
        VideoSessionInfo info;
        info.client_resized = (h[3] & 0x01u) != 0;
        info.width = read32be(h + 4);
        info.height = read32be(h + 8);
        consume(kHeaderSize);
        ++stats_.sessions;
        if (callbacks_.on_session) callbacks_.on_session(info);
        return true;
      }

      pending_pts_flags_ = read64be(h);
      pending_len_ = read32be(h + 8);
      consume(kHeaderSize);

      if (pending_len_ == 0 || pending_len_ > options_.max_packet_size) {
        fail("invalid packet length " + std::to_string(pending_len_));
        return false;
      }
      state_ = State::Payload;
      return true;
    }

    case State::Payload: {
      if (available() < pending_len_) return false;

      MediaPacket pkt;
      pkt.stream = options_.stream;
      pkt.codec = codec_;
      pkt.is_config = (pending_pts_flags_ & kFlagConfig) != 0;
      pkt.is_key_frame = (pending_pts_flags_ & kFlagKeyFrame) != 0;
      pkt.pts_us = pkt.is_config ? kNoPts
                                 : static_cast<std::int64_t>(pending_pts_flags_ & kPtsMask);
      pkt.data.assign(cursor(), cursor() + pending_len_);
      consume(pending_len_);

      ++stats_.packets;
      if (pkt.is_config) ++stats_.config_packets;
      if (pkt.is_key_frame) ++stats_.key_frames;

      state_ = State::Header;
      if (callbacks_.on_packet) callbacks_.on_packet(std::move(pkt));
      return true;
    }

    case State::Failed:
      return false;
  }
  return false;
}

}  // namespace k230
