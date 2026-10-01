#include "k230/bridge/adb_controller.hpp"

#include <sstream>

#include "k230/log.hpp"

namespace k230::bridge {

namespace {
constexpr const char* kTag = "adb";
}

AdbController::AdbController(std::string adb_path) : adb_path_(std::move(adb_path)) {}

std::vector<std::string> AdbController::base(const std::string& serial) const {
  std::vector<std::string> argv{adb_path_};
  if (!serial.empty()) {
    argv.push_back("-s");
    argv.push_back(serial);
  }
  return argv;
}

bool AdbController::available() const {
  return run_command({adb_path_, "version"}, 5000).ok();
}

std::vector<AdbDevice> AdbController::parse_devices_output(const std::string& output) {
  std::vector<AdbDevice> devices;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line.rfind("List of devices", 0) == 0 || line[0] == '*') continue;
    std::istringstream fields(line);
    AdbDevice d;
    if (!(fields >> d.serial >> d.state)) continue;
    devices.push_back(std::move(d));
  }
  return devices;
}

std::vector<AdbDevice> AdbController::devices() const {
  auto res = run_command({adb_path_, "devices"}, 10000);
  if (!res.ok()) {
    K230_LOG_WARN(kTag) << "adb devices failed (" << res.exit_code << "): " << res.output;
    return {};
  }
  return parse_devices_output(res.output);
}

std::optional<AdbDevice> AdbController::pick_device(const std::string& preferred_serial) const {
  for (const auto& d : devices()) {
    if (!preferred_serial.empty() && d.serial != preferred_serial) continue;
    if (d.usable()) return d;
    K230_LOG_WARN(kTag) << "device " << d.serial << " is '" << d.state << "' - accept the USB debugging prompt on the phone";
  }
  return std::nullopt;
}

bool AdbController::push(const std::string& serial, const std::string& local, const std::string& remote) const {
  auto argv = base(serial);
  argv.insert(argv.end(), {"push", local, remote});
  auto res = run_command(argv, 60000);
  if (!res.ok()) K230_LOG_ERROR(kTag) << "push failed: " << res.output;
  return res.ok();
}

bool AdbController::forward(const std::string& serial, std::uint16_t local_port, const std::string& remote_spec) const {
  auto argv = base(serial);
  argv.insert(argv.end(), {"forward", "tcp:" + std::to_string(local_port), remote_spec});
  auto res = run_command(argv);
  if (!res.ok()) K230_LOG_ERROR(kTag) << "forward failed: " << res.output;
  return res.ok();
}

bool AdbController::forward_remove(const std::string& serial, std::uint16_t local_port) const {
  auto argv = base(serial);
  argv.insert(argv.end(), {"forward", "--remove", "tcp:" + std::to_string(local_port)});
  return run_command(argv).ok();
}

CommandResult AdbController::shell(const std::string& serial, const std::vector<std::string>& args, int timeout_ms) const {
  auto argv = base(serial);
  argv.push_back("shell");
  argv.insert(argv.end(), args.begin(), args.end());
  return run_command(argv, timeout_ms);
}

bool AdbController::shell_async(const std::string& serial, const std::vector<std::string>& args, ChildProcess& process) const {
  auto argv = base(serial);
  argv.push_back("shell");
  argv.insert(argv.end(), args.begin(), args.end());
  return process.start(argv);
}

}  // namespace k230::bridge
