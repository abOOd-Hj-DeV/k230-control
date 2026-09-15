#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "k230/bridge/subprocess.hpp"

namespace k230::bridge {

struct AdbDevice {
  std::string serial;
  std::string state;  // "device", "unauthorized", "offline", ...
  bool usable() const { return state == "device"; }
};

// Thin wrapper over the `adb` host binary. On the K230 small core this is the
// Linux RISC-V build of adb from buildroot (or a static build); on a PC it is
// the platform-tools binary. Never calls `adb kill-server`, which would kill
// other sessions using the same daemon.
class AdbController {
 public:
  explicit AdbController(std::string adb_path = "adb");

  bool available() const;
  std::vector<AdbDevice> devices() const;
  // First device in state "device", or the one matching `preferred_serial`.
  std::optional<AdbDevice> pick_device(const std::string& preferred_serial = "") const;

  bool push(const std::string& serial, const std::string& local, const std::string& remote) const;
  bool forward(const std::string& serial, std::uint16_t local_port, const std::string& remote_spec) const;
  bool forward_remove(const std::string& serial, std::uint16_t local_port) const;
  // Runs `adb shell <args...>` to completion.
  CommandResult shell(const std::string& serial, const std::vector<std::string>& args, int timeout_ms = 15000) const;
  // Starts `adb shell <args...>` as a long-lived process.
  bool shell_async(const std::string& serial, const std::vector<std::string>& args, ChildProcess& process) const;

  static std::vector<AdbDevice> parse_devices_output(const std::string& output);

 private:
  std::vector<std::string> base(const std::string& serial) const;
  std::string adb_path_;
};

}  // namespace k230::bridge
