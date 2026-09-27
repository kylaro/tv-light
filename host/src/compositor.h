// Builds each LED frame: screen colors (or a test mode) + bass -> Pico packet.
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#include "audio_capture.h"
#include "config.h"
#include "led_layout.h"
#include "serial_link.h"
#include "video_capture.h"

namespace tvlight {

using Rgb = std::array<float, 3>;  // linear light, 0..1

// What the web UI shows. Colors are gamma encoded display RGB (not wire order).
struct Snapshot {
  std::vector<uint32_t> leds;       // 0xRRGGBB base colors before the Pico's bass mix
  std::vector<LedZone> zones;       // positions for drawing
  uint32_t floor = 0;
  float bass = 0;
  double fps = 0;
  std::vector<uint8_t> thumbnail;   // kGridW x kGridH RGB, gamma encoded
};

class Compositor {
 public:
  Compositor(ConfigStore& config, VideoCapture& video, AudioCapture& audio, SerialLink& serial);
  // Produces and sends one frame; `dt` is seconds since the previous call.
  void step(double dt);
  Snapshot snapshot() const;

 private:
  void base_colors(const Config& cfg, std::vector<Rgb>& out);
  std::vector<uint8_t> encode_packet(const Config& cfg, const Rgb& floor);

  ConfigStore& config_;
  VideoCapture& video_;
  AudioCapture& audio_;
  SerialLink& serial_;

  uint64_t layout_version_ = 0;
  LedLayout layout_;
  VideoGrid grid_;
  std::vector<Rgb> target_;
  std::vector<Rgb> smoothed_;
  double time_s_ = 0;

  mutable std::mutex mutex_;
  Snapshot snapshot_;
  uint64_t frames_ = 0;
  double fps_window_s_ = 0;
  uint64_t fps_window_frames_ = 0;
};

}  // namespace tvlight
