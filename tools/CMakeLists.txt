add_executable(k230-make-test-recording make_test_recording.cpp)
target_link_libraries(k230-make-test-recording PRIVATE k230_common k230_inspector)
add_executable(k230-layout-dump layout_dump.cpp)
target_link_libraries(k230-layout-dump PRIVATE k230_bridge)
