#include "k230/bridge/subprocess.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "k230/log.hpp"

namespace k230::bridge {

namespace {

constexpr const char* kTag = "subprocess";

// fork + exec with stdout/stderr redirected to `out_fd`. Returns pid or -1.
pid_t spawn(const std::vector<std::string>& argv, int out_fd) {
  std::vector<char*> c_argv;
  c_argv.reserve(argv.size() + 1);
  for (const auto& a : argv) c_argv.push_back(const_cast<char*>(a.c_str()));
  c_argv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    ::dup2(out_fd, STDOUT_FILENO);
    ::dup2(out_fd, STDERR_FILENO);
    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
    ::execvp(c_argv[0], c_argv.data());
    std::fprintf(stderr, "execvp(%s) failed: %s\n", c_argv[0], std::strerror(errno));
    ::_exit(127);
  }
  return pid;
}

}  // namespace

CommandResult run_command(const std::vector<std::string>& argv, int timeout_ms) {
  CommandResult result;
  int pipefd[2];
  if (::pipe(pipefd) != 0) return result;

  pid_t pid = spawn(argv, pipefd[1]);
  ::close(pipefd[1]);
  if (pid < 0) {
    ::close(pipefd[0]);
    return result;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buf[4096];
  bool timed_out = false;
  for (;;) {
    int wait_ms = -1;
    if (timeout_ms > 0) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) {
        timed_out = true;
        break;
      }
      wait_ms = static_cast<int>(remaining);
    }
    pollfd pfd{pipefd[0], POLLIN, 0};
    int r = ::poll(&pfd, 1, wait_ms);
    if (r == 0) {
      timed_out = true;
      break;
    }
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
    if (n <= 0) break;
    result.output.append(buf, static_cast<std::size_t>(n));
  }
  ::close(pipefd[0]);

  if (timed_out) {
    K230_LOG_WARN(kTag) << "command timed out: " << argv[0];
    ::kill(pid, SIGKILL);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  result.exit_code = timed_out ? -2 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  return result;
}

struct ChildProcess::State {
  pid_t pid = -1;
  int fd = -1;
  std::thread drain;
  mutable std::mutex mutex;
  std::deque<std::string> lines;
  int exit_code = -1;
  bool exited = false;
};

ChildProcess::~ChildProcess() {
  if (!state_) return;
  terminate();
  wait();
  if (state_->drain.joinable()) state_->drain.join();
  if (state_->fd >= 0) ::close(state_->fd);
  delete state_;
}

bool ChildProcess::start(const std::vector<std::string>& argv) {
  if (state_) return false;
  int pipefd[2];
  if (::pipe(pipefd) != 0) return false;
  pid_t pid = spawn(argv, pipefd[1]);
  ::close(pipefd[1]);
  if (pid < 0) {
    ::close(pipefd[0]);
    return false;
  }
  state_ = new State;
  state_->pid = pid;
  state_->fd = pipefd[0];
  State* st = state_;
  state_->drain = std::thread([st] {
    std::string partial;
    char buf[1024];
    for (;;) {
      ssize_t n = ::read(st->fd, buf, sizeof(buf));
      if (n <= 0) break;
      partial.append(buf, static_cast<std::size_t>(n));
      std::size_t pos;
      while ((pos = partial.find('\n')) != std::string::npos) {
        std::string line = partial.substr(0, pos);
        partial.erase(0, pos + 1);
        K230_LOG_DEBUG("scrcpy-server") << line;
        std::lock_guard<std::mutex> lock(st->mutex);
        st->lines.push_back(std::move(line));
        while (st->lines.size() > 200) st->lines.pop_front();
      }
    }
  });
  return true;
}

bool ChildProcess::running() const {
  if (!state_ || state_->exited) return false;
  int status = 0;
  pid_t r = ::waitpid(state_->pid, &status, WNOHANG);
  if (r == state_->pid) {
    state_->exited = true;
    state_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return false;
  }
  return r == 0;
}

void ChildProcess::terminate() {
  if (!state_ || state_->exited) return;
  ::kill(state_->pid, SIGTERM);
}

int ChildProcess::wait() {
  if (!state_) return -1;
  if (!state_->exited) {
    int status = 0;
    if (::waitpid(state_->pid, &status, 0) == state_->pid) {
      state_->exited = true;
      state_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
  }
  return state_->exit_code;
}

std::vector<std::string> ChildProcess::last_lines(std::size_t n) const {
  std::vector<std::string> out;
  if (!state_) return out;
  std::lock_guard<std::mutex> lock(state_->mutex);
  std::size_t start = state_->lines.size() > n ? state_->lines.size() - n : 0;
  out.assign(state_->lines.begin() + static_cast<std::ptrdiff_t>(start), state_->lines.end());
  return out;
}

}  // namespace k230::bridge
