#include "k230/bridge/adb_controller.hpp"

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
