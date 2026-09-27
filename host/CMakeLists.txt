cmake_minimum_required(VERSION 3.20)
project(tvlightd CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(PORTAL REQUIRED IMPORTED_TARGET libportal gio-2.0)

add_executable(tvlightd
  src/main.cc
  src/config.cc
  src/audio_capture.cc
  src/video_capture.cc
  src/portal.cc
  src/led_layout.cc
  src/compositor.cc
  src/serial_link.cc
  src/web_server.cc
)
target_include_directories(tvlightd PRIVATE src third_party ${CMAKE_CURRENT_LIST_DIR}/../protocol)
target_compile_options(tvlightd PRIVATE -Wall -Wextra)
target_compile_definitions(tvlightd PRIVATE TVLIGHT_SOURCE_WEB_DIR="${CMAKE_CURRENT_LIST_DIR}/web")
target_link_libraries(tvlightd PRIVATE PkgConfig::PIPEWIRE PkgConfig::PORTAL Threads::Threads)

add_executable(dsp_test tests/dsp_test.cc)
target_include_directories(dsp_test PRIVATE src)
