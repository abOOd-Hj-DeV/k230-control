#include "k230/bridge/tcp_socket.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace k230::bridge {

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : fd_(other.fd_), last_receive_timed_out_(other.last_receive_timed_out_) {
  other.fd_ = -1;
  other.last_receive_timed_out_ = false;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    last_receive_timed_out_ = other.last_receive_timed_out_;
    other.fd_ = -1;
    other.last_receive_timed_out_ = false;
  }
  return *this;
}

bool TcpSocket::connect(const std::string& host, std::uint16_t port, int timeout_ms) {
  close();
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return false;
  }

  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (r != 0 && errno != EINPROGRESS) {
    ::close(fd);
    return false;
  }
  if (r != 0) {
    pollfd pfd{fd, POLLOUT, 0};
    if (::poll(&pfd, 1, timeout_ms) <= 0) {
      ::close(fd);
      return false;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
      ::close(fd);
      return false;
    }
  }
  ::fcntl(fd, F_SETFL, flags);
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  fd_ = fd;
  last_receive_timed_out_ = false;
  return true;
}

void TcpSocket::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  last_receive_timed_out_ = false;
}

bool TcpSocket::set_send_timeout(int timeout_ms) {
  if (fd_ < 0 || timeout_ms <= 0) return false;
  const timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  return ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

void TcpSocket::shutdown() {
  if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
}

long TcpSocket::recv(std::uint8_t* buf, std::size_t len, int timeout_ms) {
  last_receive_timed_out_ = false;
  if (fd_ < 0) return -1;
  if (timeout_ms >= 0) {
    pollfd pfd{fd_, POLLIN, 0};
    int r = ::poll(&pfd, 1, timeout_ms);
    if (r == 0) {
      last_receive_timed_out_ = true;
      return -1;
    }
    if (r < 0) return -1;
  }
  for (;;) {
    ssize_t n = ::recv(fd_, buf, len, 0);
    if (n < 0 && errno == EINTR) continue;
    return static_cast<long>(n);
  }
}

bool TcpSocket::recv_exact(std::uint8_t* buf, std::size_t len, int timeout_ms) {
  std::size_t got = 0;
  while (got < len) {
    long n = recv(buf + got, len - got, timeout_ms);
    if (n <= 0) return false;
    got += static_cast<std::size_t>(n);
  }
  return true;
}

bool TcpSocket::send_all(const std::uint8_t* buf, std::size_t len) {
  if (fd_ < 0) return false;
  std::size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd_, buf + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool TcpSocket::send_all(const std::string& s) {
  return send_all(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

}  // namespace k230::bridge
