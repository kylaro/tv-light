// Core 0: parses host packets from USB CDC and hands them to the renderer.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "frame_store.h"
#include "tvlight_protocol.h"

namespace tvlight::fw {

class FrameReceiver {
 public:
  explicit FrameReceiver(FrameStore& store) : store_(store) {}
  // Reads whatever USB has buffered (waits up to `timeout_us`) and parses it.
  // Returns the number of valid packets completed.
  uint32_t poll(uint32_t timeout_us);

  uint32_t frames_ok() const { return frames_ok_; }
  uint32_t bass_ok() const { return bass_ok_; }
  uint32_t packets_bad() const { return packets_bad_; }
  uint16_t led_count() const { return led_count_; }

 private:
  bool feed(uint8_t byte);
  bool accept_packet();
  void accept_frame();
  void accept_bass();
  void rebuild_gamma(uint8_t gamma_tenths);

  static constexpr size_t kMaxPacket = protocol::frame_packet_size(protocol::kMaxLeds);
  static constexpr size_t kReadChunk = 256;

  FrameStore& store_;
  std::array<uint8_t, kMaxPacket> packet_{};
  size_t pos_ = 0;
  size_t expected_ = 0;
  std::array<uint16_t, 256> gamma_lut_{};
  uint8_t gamma_tenths_ = 0;
  uint32_t frames_ok_ = 0;
  uint32_t bass_ok_ = 0;
  uint32_t packets_bad_ = 0;
  uint16_t led_count_ = 0;
};

}  // namespace tvlight::fw
