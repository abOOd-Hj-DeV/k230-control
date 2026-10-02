#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <thread>
#include <chrono>

#include "k230/bridge/layout_receiver.hpp"
#include "k230/cli.hpp"
#include "k230/layout.hpp"

int main(int argc, char** argv) {
  k230::Cli cli(argc, argv);
  if (cli.has("read")) {
    std::ifstream input(cli.get("read"), std::ios::binary);
    if (!input) return 1;
    std::array<std::uint8_t, 4> prefix;
    while (input.read(reinterpret_cast<char*>(prefix.data()), prefix.size())) {
      const auto size = k230::layout_payload_size(prefix.data());
      if (size < k230::kLayoutHeaderSize || size > k230::kLayoutMaxPayload) return 1;
      std::vector<std::uint8_t> payload(size);
      if (!input.read(reinterpret_cast<char*>(payload.data()), size)) return 1;
      auto s = k230::decode_layout(payload.data(), size);
      if (!s) return 1;
      std::puts(k230::describe_layout(*s).c_str());
    }
    return input.eof() && input.gcount() == 0 ? 0 : 1;
  }
  k230::bridge::AdbController adb(cli.get("adb", "adb"));
  const auto device = adb.pick_device(cli.get("serial", ""));
  if (!device) return 1;
  auto cache = std::make_shared<k230::LayoutCache>();
  k230::bridge::LayoutReceiverConfig config;
  config.recording = cli.get("record", "");
  k230::bridge::LayoutReceiver receiver(config, adb, device->serial, cache);
  receiver.set_observer([](const auto& s) { std::puts(k230::describe_layout(s).c_str()); });
  receiver.start();
  std::this_thread::sleep_for(std::chrono::seconds(cli.get_int("duration", 5)));
  receiver.stop();
}
