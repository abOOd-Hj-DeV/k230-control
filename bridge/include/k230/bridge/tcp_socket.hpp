#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace k230::bridge {

// Minimal blocking TCP client. Used for the adb-forwarded scrcpy sockets and
// for the companion-app verdict socket.
class TcpSocket {
 public:
  TcpSocket() = default;
  ~TcpSocket();
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;
  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;

  bool connect(const std::string& host, std::uint16_t port, int timeout_ms = 2000);
  bool connected() const { return fd_ >= 0; }
  bool set_send_timeout(int timeout_ms);
  void close();

  // Returns bytes read (>0), 0 on orderly shutdown, -1 on error / timeout.
  long recv(std::uint8_t* buf, std::size_t len, int timeout_ms = -1);
  bool recv_exact(std::uint8_t* buf, std::size_t len, int timeout_ms = -1);
  bool send_all(const std::uint8_t* buf, std::size_t len);
  bool send_all(const std::string& s);

  // Wake up a blocked recv() from another thread.
  void shutdown();

 private:
  int fd_ = -1;
};

}  // namespace k230::bridge
