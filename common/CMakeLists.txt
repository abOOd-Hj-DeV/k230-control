add_library(k230_common STATIC
  src/scrcpy_demuxer.cpp
  src/recording.cpp
  src/verdict.cpp
  src/layout.cpp
  src/ipc/datafifo_channel.cpp
)

target_include_directories(k230_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

find_package(Threads REQUIRED)
target_link_libraries(k230_common PUBLIC Threads::Threads)
