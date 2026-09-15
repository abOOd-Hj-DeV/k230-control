#pragma once

#include <string>
#include <vector>

#include <sys/types.h>

namespace k230::bridge {

struct CommandResult {
  int exit_code = -1;
  std::string output;  // stdout + stderr
  bool ok() const { return exit_code == 0; }
};

// Run `argv` to completion and capture its output. `timeout_ms <= 0` waits forever.
CommandResult run_command(const std::vector<std::string>& argv, int timeout_ms = 15000);

// A long-lived child process (the scrcpy server running under `adb shell`).
// Its output is drained by a background thread into `last_lines()` so that the
// pipe never fills up and server errors can be reported.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  bool start(const std::vector<std::string>& argv);
  bool running() const;
  void terminate();
  int wait();  // returns exit code, -1 if never started
  std::vector<std::string> last_lines(std::size_t n = 20) const;

 private:
  struct State;
  State* state_ = nullptr;
};

}  // namespace k230::bridge
