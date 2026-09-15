set(INSPECTOR_SOURCES
  src/pcm_ring.cpp
  src/sync_engine.cpp
  src/analyzer.cpp
  src/policy.cpp
  src/pipeline.cpp
  src/frame_dump.cpp
  src/raw_pcm_decoder.cpp
  src/vdec_decoder.cpp
  src/kpu_analyzer.cpp
)

add_library(k230_inspector STATIC ${INSPECTOR_SOURCES})
target_include_directories(k230_inspector PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(k230_inspector PUBLIC k230_common)

# Software decoders exist only on the PC; the big core uses the VDEC hardware.
if(K230_TARGET STREQUAL "pc")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
  target_sources(k230_inspector PRIVATE src/ffmpeg_decoder.cpp)
  target_link_libraries(k230_inspector PUBLIC PkgConfig::FFMPEG)
  target_compile_definitions(k230_inspector PUBLIC K230_HAS_FFMPEG=1)
endif()

# Big-core executable. Reads packets from DATAFIFO (K230) or a .k230rec file
# (PC), decodes, synchronises, analyses and emits verdicts.
add_executable(k230-inspector src/main.cpp)
target_link_libraries(k230-inspector PRIVATE k230_inspector)
