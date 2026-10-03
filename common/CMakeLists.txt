add_library(k230_common STATIC
  src/scrcpy_demuxer.cpp
  src/recording.cpp
  src/verdict.cpp
  src/layout.cpp
  src/companion_v2.cpp
  src/ipc/datafifo_channel.cpp
)

target_include_directories(k230_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>
)

file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nlohmann/json.hpp" JSON_HEADER_SHA256)
if(NOT JSON_HEADER_SHA256 STREQUAL "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6")
  message(FATAL_ERROR "Pinned nlohmann/json 3.11.3 header checksum mismatch")
endif()

find_package(Threads REQUIRED)
target_link_libraries(k230_common PUBLIC Threads::Threads)
