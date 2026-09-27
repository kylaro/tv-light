cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD firefly_v1_1 CACHE STRING "Board header (firefly_v1_1, pico, ...)")
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
# Local SDK copy if present (external/ is gitignored); otherwise fetch the same tag.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/external/pico-sdk)
  set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/external/pico-sdk CACHE PATH "Pico SDK")
else()
  set(PICO_SDK_FETCH_FROM_GIT ON)
  set(PICO_SDK_FETCH_FROM_GIT_TAG "2.1.1")
endif()
# Prebuilt picotool if present so the SDK does not fetch and build one.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/external/picotool)
  set(picotool_DIR ${CMAKE_CURRENT_LIST_DIR}/external/picotool CACHE PATH "picotool package dir")
endif()
include(pico_sdk_import.cmake)

project(tvlight_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(tvlight
  src/main.cc
  src/frame_receiver.cc
  src/renderer.cc
  src/ws2812.cc
  src/frame_store.cc
)
pico_generate_pio_header(tvlight ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)
target_include_directories(tvlight PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../protocol)
target_compile_options(tvlight PRIVATE -Wall -Wextra -O2)
target_compile_definitions(tvlight PRIVATE
  USBD_PRODUCT="TV Light"
  PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
)
target_link_libraries(tvlight pico_stdlib pico_multicore hardware_pio hardware_dma)
pico_enable_stdio_usb(tvlight 1)
pico_enable_stdio_uart(tvlight 0)
pico_add_extra_outputs(tvlight)
