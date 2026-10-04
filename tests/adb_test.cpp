#include "k230/bridge/adb_controller.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

using namespace k230::bridge;

TEST(AdbController, ParsesDevicesOutput) {
  const std::string out =
      "* daemon not running; starting now at tcp:5037\n"
      "* daemon started successfully\n"
      "List of devices attached\n"
      "R58M12ABCDE\tdevice\n"
      "emulator-5554\toffline\n"
      "0123456789ABCDEF\tunauthorized\n"
      "\n";
  auto devices = AdbController::parse_devices_output(out);
  ASSERT_EQ(devices.size(), 3u);
  EXPECT_EQ(devices[0].serial, "R58M12ABCDE");
  EXPECT_TRUE(devices[0].usable());
  EXPECT_EQ(devices[1].state, "offline");
  EXPECT_FALSE(devices[1].usable());
  EXPECT_EQ(devices[2].state, "unauthorized");
  EXPECT_FALSE(devices[2].usable());
}

TEST(AdbController, EmptyList) {
  EXPECT_TRUE(AdbController::parse_devices_output("List of devices attached\n\n").empty());
}

class NetworkAdb : public testing::Test {
 protected:
  void SetUp() override {
    const auto pattern = (std::filesystem::temp_directory_path() / "k230_adb_XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    ASSERT_NE(::mkdtemp(name.data()), nullptr);
    directory_ = name.data();
    executable_ = directory_ / "adb";
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }
  void script(const std::string& state, int connect_exit = 0) {
    std::ofstream file(executable_);
    file << "#!/bin/sh\n"
         << "if [ \"$#\" -eq 2 ] && [ \"$1\" = connect ] && [ \"$2\" = 100.64.0.2:37123 ]; then\n"
         << "  echo 'connected to 100.64.0.2:37123'; exit " << connect_exit << "\nfi\n"
         << "if [ \"$#\" -eq 3 ] && [ \"$1\" = -s ] && [ \"$2\" = 100.64.0.2:37123 ] && [ \"$3\" = get-state ]; then\n"
         << "  echo '" << state << "'; exit 0\nfi\nexit 2\n";
    file.close();
    std::filesystem::permissions(executable_, std::filesystem::perms::owner_all);
  }
  std::filesystem::path directory_, executable_;
};

TEST_F(NetworkAdb, ConnectsAndChecksTheSpecifiedDevice) {
  script("device");
  EXPECT_TRUE(AdbController(executable_.string()).connect("100.64.0.2:37123"));
}

TEST_F(NetworkAdb, ZeroExitConnectDoesNotAuthorizeAnOfflineDevice) {
  script("offline");
  EXPECT_FALSE(AdbController(executable_.string()).connect("100.64.0.2:37123"));
}

TEST_F(NetworkAdb, RejectsFailedConnectionEvenWhenGetStateWouldSucceed) {
  script("device", 1);
  EXPECT_FALSE(AdbController(executable_.string()).connect("100.64.0.2:37123"));
}
