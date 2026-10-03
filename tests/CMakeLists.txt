find_package(GTest REQUIRED)

add_executable(k230_tests
  demuxer_test.cpp
  wire_test.cpp
  layout_test.cpp
  queue_test.cpp
  pcm_ring_test.cpp
  frame_dump_test.cpp
  continuous_capture_test.cpp
  sync_engine_test.cpp
  policy_test.cpp
  nsfwjs_test.cpp
  adb_test.cpp
  tcp_socket_test.cpp
  scrcpy_stream_test.cpp
)
target_link_libraries(k230_tests PRIVATE k230_bridge k230_inspector GTest::gtest GTest::gtest_main)
target_compile_definitions(k230_tests PRIVATE
  K230_TEST_NSFWJS_MODEL="${PROJECT_SOURCE_DIR}/models/nsfwjs-mobilenet-v2.onnx")
target_compile_definitions(k230_tests PRIVATE K230_LAYOUT_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/layout-kotlin-v1.bin")
target_compile_definitions(k230_tests PRIVATE K230_LAYOUT_LIVE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/layout-android-api35.bin")

include(GoogleTest)
gtest_discover_tests(k230_tests)
