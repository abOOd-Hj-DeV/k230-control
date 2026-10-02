cmake_minimum_required(VERSION 3.16)
project(k230_control LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# K230_TARGET selects the deployment flavour:
#   pc      - development host; FFmpeg software decoder, both cores emulated in one process
#   little  - K230 small core (Linux/buildroot): bridge only, no FFmpeg
#   big     - K230 big core (RT-Smart): inspector only, VDEC + KPU backends
set(K230_TARGET "pc" CACHE STRING "Deployment target: pc | little | big")
set_property(CACHE K230_TARGET PROPERTY STRINGS pc little big)

option(K230_BUILD_TESTS "Build unit tests" ON)
option(K230_ENABLE_ONNX "Enable the PC NSFWJS analyzer using ONNX Runtime" OFF)
set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime C++ SDK prefix (include/ and lib/)")

if(K230_ENABLE_ONNX AND NOT K230_TARGET STREQUAL "pc")
  message(FATAL_ERROR "ONNX Runtime is a PC evaluation backend; K230 uses nncase/KPU")
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_subdirectory(common)

if(K230_TARGET STREQUAL "pc" OR K230_TARGET STREQUAL "little")
  add_subdirectory(bridge)
endif()

if(K230_TARGET STREQUAL "pc" OR K230_TARGET STREQUAL "big")
  add_subdirectory(inspector)
endif()

if(K230_TARGET STREQUAL "pc")
  add_subdirectory(apps)
  add_subdirectory(tools)
endif()

if(K230_BUILD_TESTS AND K230_TARGET STREQUAL "pc")
  enable_testing()
  add_subdirectory(tests)
endif()
