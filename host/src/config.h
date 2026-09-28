// Persistent, web-editable settings. Stored as JSON; every module reads a
// snapshot via ConfigStore::get() and watches version() for changes.
#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "json.hpp"

namespace tvlight {

struct LayoutConfig {
  // LEDs per segment in strip order, viewed from the front of the TV: the strip
  // starts at the bottom middle and runs clockwise (bottom left half, left side
  // upwards, top rightwards, right side downwards, bottom right half).
  int skip = 0;              // leading strip LEDs before the bottom middle; kept dark
  int bottom_left = 20;
  int left = 22;
  int top = 40;
  int right = 22;
  int bottom_right = 20;
  double depth = 0.15;       // sampling band depth, fraction of screen height/width
  double bottom_gap = 0.0;   // centered fraction of the bottom edge without LEDs (TV stand)
};

struct BassConfig {
  bool enabled = true;
  double lowpass_hz = 50.0;
  double highpass_hz = 20.0;
  double attack_ms = 5.0;
  double release_ms = 150.0;
  bool agc = true;             // normalize against the recent peak
  double agc_decay_s = 8.0;
  double sensitivity = 4.0;    // manual gain when agc is off
  double threshold = 0.15;     // envelope below this reads as 0
  double curve = 1.5;          // exponent after thresholding; >1 makes kicks punchier
  double share = 0.4;          // part of brightness driven by bass; the rest is always on
  double smoothing_ms = 30.0;  // exponential moving average on the final bass value; 0 = off
  double floor = 0.15;         // adds floor * bass of the scene hue so dark scenes still thump
};

struct Config {
  std::string mode = "ambient";          // ambient | solid | rainbow | off
  std::string solid_color = "#ff7a18";
  std::string capture_source = "auto";   // auto | gamescope | portal | none
  std::string portal_restore_token;
  std::string serial_port = "auto";
  std::string color_order = "GRB";
  int led_count = 124;                   // physical strip length; LEDs past the layout stay dark
  int brightness = 200;                  // 0..255
  int max_current_ma = 2000;             // strip budget; 0 = unlimited
  int fps = 60;                          // LED frames per second (bass pump rate)
  // Audio block in samples at 48 kHz: sets the bass update rate (256 -> ~188 Hz).
  // Smaller blocks also shrink the whole PipeWire graph quantum while we run.
  int audio_block = 256;
  int capture_fps = 60;                  // screen frames per second; each one costs the compositor a full copy
  int web_port = 8080;
  double screen_gamma = 2.2;             // decode of screen colors; higher = more contrast
  double saturation = 1.2;
  double smoothing_ms = 60.0;
  double white_r = 1.0;
  double white_g = 1.0;
  double white_b = 1.0;
  bool dither = true;
  bool interpolate = true;
  LayoutConfig layout;
  BassConfig bass;
};

// Missing keys keep their defaults, so old config files keep loading.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LayoutConfig, skip, bottom_left, left, top, right, bottom_right, depth,
                                                bottom_gap)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(BassConfig, enabled, lowpass_hz, highpass_hz, attack_ms, release_ms,
                                                agc, agc_decay_s, sensitivity, threshold, curve, share,
                                                smoothing_ms, floor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, mode, solid_color, capture_source, portal_restore_token,
                                                serial_port, color_order, led_count, brightness, max_current_ma, fps, capture_fps, audio_block,
                                                web_port,
                                                screen_gamma, saturation, smoothing_ms, white_r, white_g, white_b,
                                                dither, interpolate, layout, bass)

class ConfigStore {
 public:
  explicit ConfigStore(std::string path);
  void load();
  Config get() const;
  uint64_t version() const { return version_.load(); }
  // Applies a JSON merge patch, clamps values, saves. Returns the new config.
  Config patch(const nlohmann::json& patch);
  void set_restore_token(const std::string& token);
  const std::string& path() const { return path_; }

 private:
  void save_locked() const;

  std::string path_;
  mutable std::mutex mutex_;
  Config config_;
  std::atomic<uint64_t> version_{1};
};

}  // namespace tvlight
