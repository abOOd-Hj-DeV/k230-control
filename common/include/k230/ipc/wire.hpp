#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "k230/media_packet.hpp"
#include "k230/verdict.hpp"

namespace k230::ipc {

// Byte encoding of MediaPacket / Verdict used everywhere the two cores (or a
// recording file) exchange data. Fixed-size big-endian header followed by the
// payload, so it can be written straight into a DATAFIFO slot or a file.
//
//   MediaPacket header (24 bytes)
//     0     u8   magic 'M'
//     1     u8   stream   (StreamType)
//     2     u8   flags    bit0 config, bit1 key frame
//     3     u8   reserved
//     4..7  u32  codec id
//     8..15 i64  pts_us   (kNoPts for config packets)
//    16..19 u32  payload length
//    20..23 u32  reserved
//
//   Verdict (24 bytes, no payload)
//     0     u8   magic 'V'
//     1     u8   action
//     2     u8   category
//     3     u8   reserved
//     4..7  u32  sequence
//     8..15 i64  pts_us
//    16..19 u32  confidence * 1e6
//    20..23 u32  reserved
constexpr std::size_t kPacketHeaderSize = 24;
constexpr std::size_t kVerdictSize = 24;

void encode_header(const MediaPacket& p, std::uint8_t out[kPacketHeaderSize]);
std::vector<std::uint8_t> encode(const MediaPacket& p);

// Parses a header; returns the payload length or nullopt if invalid.
std::optional<std::uint32_t> decode_header(const std::uint8_t in[kPacketHeaderSize], MediaPacket& out);

void encode(const Verdict& v, std::uint8_t out[kVerdictSize]);
std::optional<Verdict> decode_verdict(const std::uint8_t in[kVerdictSize]);

}  // namespace k230::ipc
