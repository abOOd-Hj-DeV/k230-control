add_library(k230_bridge STATIC
  src/subprocess.cpp
  src/tcp_socket.cpp
  src/adb_controller.cpp
  src/scrcpy_session.cpp
  src/verdict_dispatcher.cpp
  src/layout_receiver.cpp
)

target_include_directories(k230_bridge PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(k230_bridge PUBLIC k230_common)

# Little-core executable. On the K230 it feeds DATAFIFO; on a PC it can record
# the phone stream to a file for offline inspector development.
add_executable(k230-bridge src/main.cpp)
target_link_libraries(k230-bridge PRIVATE k230_bridge)
