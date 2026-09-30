#include "k230/ipc/datafifo_channel.hpp"

#include <algorithm>
#include <stdexcept>

#include "k230/byteorder.hpp"
#include "k230/ipc/wire.hpp"

namespace k230::ipc {

// ---------------------------------------------------------------------------
// Wire encoding (shared by DATAFIFO, recordings and tests)
// ---------------------------------------------------------------------------

void encode_header(const MediaPacket& p, std::uint8_t out[kPacketHeaderSize]) {
  out[0] = 'M';
  out[1] = static_cast<std::uint8_t>(p.stream);
  out[2] = static_cast<std::uint8_t>((p.is_config ? 1u : 0u) | (p.is_key_frame ? 2u : 0u));
  out[3] = 0;
  write32be(out + 4, static_cast<std::uint32_t>(p.codec));
  write64be(out + 8, static_cast<std::uint64_t>(p.pts_us));
  write32be(out + 16, static_cast<std::uint32_t>(p.data.size()));
  write32be(out + 20, p.seq);
}

std::vector<std::uint8_t> encode(const MediaPacket& p) {
  std::vector<std::uint8_t> out(kPacketHeaderSize + p.data.size());
  encode_header(p, out.data());
  std::copy(p.data.begin(), p.data.end(), out.begin() + kPacketHeaderSize);
  return out;
}

std::optional<std::uint32_t> decode_header(const std::uint8_t in[kPacketHeaderSize], MediaPacket& out) {
  if (in[0] != 'M') return std::nullopt;
  if (in[1] > static_cast<std::uint8_t>(StreamType::Audio)) return std::nullopt;
  out.stream = static_cast<StreamType>(in[1]);
  out.is_config = (in[2] & 1u) != 0;
  out.is_key_frame = (in[2] & 2u) != 0;
  out.codec = static_cast<CodecId>(read32be(in + 4));
  out.pts_us = static_cast<std::int64_t>(read64be(in + 8));
  out.seq = read32be(in + 20);
  return read32be(in + 16);
}

void encode(const Verdict& v, std::uint8_t out[kVerdictSize]) {
  out[0] = 'V';
  out[1] = static_cast<std::uint8_t>(v.action);
  out[2] = static_cast<std::uint8_t>(v.category);
  out[3] = 0;
  write32be(out + 4, v.sequence);
  write64be(out + 8, static_cast<std::uint64_t>(v.pts_us));
  write32be(out + 16, static_cast<std::uint32_t>(v.confidence * 1e6f));
  write32be(out + 20, 0);
}

std::optional<Verdict> decode_verdict(const std::uint8_t in[kVerdictSize]) {
  if (in[0] != 'V') return std::nullopt;
  Verdict v;
  v.action = static_cast<Action>(in[1]);
  v.category = static_cast<Category>(in[2]);
  v.sequence = read32be(in + 4);
  v.pts_us = static_cast<std::int64_t>(read64be(in + 8));
  v.confidence = static_cast<float>(read32be(in + 16)) / 1e6f;
  return v;
}

// ---------------------------------------------------------------------------
// K230 SDK backends
// ---------------------------------------------------------------------------

#ifdef K230_HAS_CDK
// TODO(k230): implement on top of k_datafifo_* / k_ipcmsg_* from the K230 SDK.
//
// Sketch (little core, writer):
//   k_datafifo_params_s params{cfg.item_count, cfg.item_size, K_TRUE /*release by reader*/, DATAFIFO_WRITER};
//   k_datafifo_open_by_addr(&handle_, &params, cfg.phys_addr);
//   k_datafifo_cmd(handle_, DATAFIFO_CMD_SET_DATA_RELEASE_CALLBACK, release_cb);
//   push(): serialise with encode_header + payload into one slot, k_datafifo_write(handle_, buf);
//           k_datafifo_cmd(handle_, DATAFIFO_CMD_WRITE_DONE, nullptr);
//
// Sketch (big core, reader):
//   k_datafifo_open_by_addr(&handle_, &params /*DATAFIFO_READER*/, cfg.phys_addr);
//   pop(): k_datafifo_read(handle_, &slot); decode_header; copy payload; k_datafifo_cmd(handle_, DATAFIFO_CMD_READ_DONE, slot);
//
// Verdicts: k_ipcmsg_connect / k_ipcmsg_create_message(kVerdictSize bytes) / k_ipcmsg_send_async.
#error "K230_HAS_CDK backend not implemented yet; see sketch above"
#else

namespace {
[[noreturn]] void not_available(const char* what) {
  throw std::runtime_error(std::string(what) + " is only available when building inside the K230 SDK (K230_HAS_CDK)");
}
}  // namespace

struct DataFifoPacketSink::Impl {};
DataFifoPacketSink::DataFifoPacketSink(const DataFifoConfig&) { not_available("DataFifoPacketSink"); }
DataFifoPacketSink::~DataFifoPacketSink() = default;
bool DataFifoPacketSink::push(MediaPacket&&) { return false; }
void DataFifoPacketSink::close() {}

struct DataFifoPacketSource::Impl {};
DataFifoPacketSource::DataFifoPacketSource(const DataFifoConfig&) { not_available("DataFifoPacketSource"); }
DataFifoPacketSource::~DataFifoPacketSource() = default;
std::optional<MediaPacket> DataFifoPacketSource::pop(std::chrono::milliseconds) { return std::nullopt; }
bool DataFifoPacketSource::closed() const { return true; }

struct IpcMsgVerdictSink::Impl {};
IpcMsgVerdictSink::IpcMsgVerdictSink(const IpcMsgConfig&) { not_available("IpcMsgVerdictSink"); }
IpcMsgVerdictSink::~IpcMsgVerdictSink() = default;
bool IpcMsgVerdictSink::push(Verdict&&) { return false; }
void IpcMsgVerdictSink::close() {}

struct IpcMsgVerdictSource::Impl {};
IpcMsgVerdictSource::IpcMsgVerdictSource(const IpcMsgConfig&) { not_available("IpcMsgVerdictSource"); }
IpcMsgVerdictSource::~IpcMsgVerdictSource() = default;
std::optional<Verdict> IpcMsgVerdictSource::pop(std::chrono::milliseconds) { return std::nullopt; }
bool IpcMsgVerdictSource::closed() const { return true; }

#endif  // K230_HAS_CDK

}  // namespace k230::ipc
