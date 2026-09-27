// Core 1: interpolates between host frames, mixes in bass, limits current,
// dithers to 8 bits and drives the strip as fast as it can latch.
#pragma once

#include <array>
#include <cstdint>

#include "frame_store.h"
#include "ws2812.h"

namespace tvlight::fw {

class Renderer {
 public:
  explicit Renderer(FrameStore& store) : store_(store) {}
  void init(unsigned pin);
  [[noreturn]] void run();

  // Read by core 0 for the status line.
  volatile uint32_t renders = 0;
  volatile uint32_t last_milliamps = 0;
  volatile uint32_t limited_frames = 0;

 private:
  void pick_up_frame(uint64_t now_us);
  void pick_up_bass(uint64_t now_us);
  uint16_t current_bass(uint64_t now_us);
  void render(uint64_t now_us, uint32_t* out_words);

  FrameStore& store_;
  Ws2812 strip_;
  uint32_t seq_ = 0;

  LinearFrame incoming_;
  // Interpolation endpoints (base colors in linear light, before mixing).
  LinearFrame from_;
  LinearFrame to_;
  std::array<uint16_t, kChannels> shown_{};
  // Bass ramps from the value shown when the newest packet arrived toward it.
  uint32_t bass_word_ = 0;
  uint16_t bass_from_ = 0;
  uint16_t bass_to_ = 0;
  uint16_t shown_bass_ = 0;
  uint64_t bass_start_us_ = 0;
  uint64_t last_bass_us_ = 0;
  uint32_t bass_interp_us_ = kDefaultBassInterpUs;
  std::array<uint16_t, protocol::kBytesPerLed> shown_floor_{};

  uint64_t interp_start_us_ = 0;
  uint32_t interp_len_us_ = kDefaultInterpUs;
  uint64_t last_frame_us_ = 0;
  bool have_frame_ = false;
  uint16_t transmit_count_ = 0;

  std::array<uint32_t, kChannels> mixed_{};      // linear, can exceed 16 bits pre-limit
  std::array<uint8_t, kChannels> dither_err_{};
  std::array<uint32_t, protocol::kMaxLeds> words_[2]{};
};

}  // namespace tvlight::fw
