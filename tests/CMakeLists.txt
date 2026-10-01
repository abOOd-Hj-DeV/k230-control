find_package(GTest REQUIRED)

add_executable(k230_tests
  demuxer_test.cpp
  wire_test.cpp
  queue_test.cpp
  pcm_ring_test.cpp
  frame_dump_test.cpp
  sync_engine_test.cpp
  policy_test.cpp
  adb_test.cpp
  tcp_socket_test.cpp
  scrcpy_stream_test.cpp
)
target_link_libraries(k230_tests PRIVATE k230_bridge k230_inspector GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(k230_tests)
