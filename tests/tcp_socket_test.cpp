#include <chrono>
#include <cstdint>
#include <vector>
#include <future>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "k230/bridge/tcp_socket.hpp"

using namespace k230::bridge;

TEST(TcpSocket, DistinguishesTimeoutFromDisconnectedPeer) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listener, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(listener, 1), 0);
  socklen_t address_size = sizeof(address);
  ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size), 0);

  TcpSocket client;
  ASSERT_TRUE(client.connect("127.0.0.1", ntohs(address.sin_port)));
  const int peer = ::accept(listener, nullptr, nullptr);
  ASSERT_GE(peer, 0);

  std::uint8_t byte = 0;
  EXPECT_EQ(client.recv(&byte, 1, 20), -1);
  EXPECT_TRUE(client.last_receive_timed_out());
  ASSERT_EQ(::send(peer, "x", 1, 0), 1);
  EXPECT_EQ(client.recv(&byte, 1, 100), 1);
  EXPECT_FALSE(client.last_receive_timed_out());
  EXPECT_EQ(byte, static_cast<std::uint8_t>('x'));
  ::close(peer);
  EXPECT_EQ(client.recv(&byte, 1, 100), 0);
  EXPECT_FALSE(client.last_receive_timed_out());

  client.close();
  ::close(listener);
}

TEST(TcpSocket, SendReturnsWhenPeerStopsReading) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listener, 0);
  const int receive_buffer = 4096;
  ASSERT_EQ(::setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)), 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(listener, 1), 0);
  socklen_t address_size = sizeof(address);
  ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size), 0);

  TcpSocket client;
  ASSERT_TRUE(client.connect("127.0.0.1", ntohs(address.sin_port)));
  const int peer = ::accept(listener, nullptr, nullptr);
  ASSERT_GE(peer, 0);
  ASSERT_TRUE(client.set_send_timeout(100));

  const std::vector<std::uint8_t> payload(16 * 1024 * 1024, 42);
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(client.send_all(payload.data(), payload.size()));
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));

  client.close();
  ::close(peer);
  ::close(listener);
}

TEST(TcpSocket, TrickleBytesCannotRenewAbsoluteReadDeadline) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listener, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(listener, 1), 0);
  socklen_t size = sizeof(address);
  ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size), 0);
  TcpSocket client;
  ASSERT_TRUE(client.connect("127.0.0.1", ntohs(address.sin_port)));
  const int peer = ::accept(listener, nullptr, nullptr);
  ASSERT_GE(peer, 0);
  auto sender = std::async(std::launch::async, [&] {
    for (int i = 0; i < 10; ++i) {
      if (::send(peer, "x", 1, MSG_NOSIGNAL) != 1) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
  });
  std::uint8_t bytes[10]{};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(client.recv_exact(bytes, sizeof(bytes), 100));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(250));
  client.close(); sender.get(); ::close(peer); ::close(listener);
}
