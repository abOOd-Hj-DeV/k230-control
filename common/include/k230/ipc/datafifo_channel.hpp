#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "k230/ipc/channel.hpp"

namespace k230::ipc {

// K230 SDK transports between the Linux small core and the RT-Smart big core.
//
//   DATAFIFO  bulk data (compressed media packets, little -> big)
//   IPCMSG    small control messages (verdicts, big -> little)
//
// Both are provided by the K230 SDK (`src/common/cdk/user/component/datafifo`
// and `.../ipcmsg`), and are only available when building inside the SDK
// (K230_HAS_CDK defined by the buildroot / RT-Smart toolchain files).
//
// On a PC build every constructor throws std::runtime_error so that wiring
// mistakes are caught immediately; the in-process queues from channel.hpp
// are used instead.
struct DataFifoConfig {
  // Physical address of the shared DATAFIFO region. Both cores must agree;
  // it is normally read from the device tree / SDK config header.
  std::uint64_t phys_addr = 0;
  std::uint32_t item_count = 64;      // ring depth (number of slots)
  std::uint32_t item_size = 256 * 1024;  // bytes per slot; must hold the largest H.264 key frame
};

class DataFifoPacketSink final : public PacketSink {
 public:
  explicit DataFifoPacketSink(const DataFifoConfig& cfg);
  ~DataFifoPacketSink() override;
  bool push(MediaPacket&& item) override;
  void close() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class DataFifoPacketSource final : public PacketSource {
 public:
  explicit DataFifoPacketSource(const DataFifoConfig& cfg);
  ~DataFifoPacketSource() override;
  std::optional<MediaPacket> pop(std::chrono::milliseconds timeout) override;
  bool closed() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct IpcMsgConfig {
  std::string service_name = "k230_verdict";
  std::int32_t node_id = 0;  // 0 = little core, 1 = big core (SDK convention)
};

class IpcMsgVerdictSink final : public VerdictSink {
 public:
  explicit IpcMsgVerdictSink(const IpcMsgConfig& cfg);
  ~IpcMsgVerdictSink() override;
  bool push(Verdict&& item) override;
  void close() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class IpcMsgVerdictSource final : public VerdictSource {
 public:
  explicit IpcMsgVerdictSource(const IpcMsgConfig& cfg);
  ~IpcMsgVerdictSource() override;
  std::optional<Verdict> pop(std::chrono::milliseconds timeout) override;
  bool closed() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace k230::ipc
