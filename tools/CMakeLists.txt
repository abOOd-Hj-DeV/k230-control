add_executable(k230-make-test-recording make_test_recording.cpp)
target_link_libraries(k230-make-test-recording PRIVATE k230_common k230_inspector)
