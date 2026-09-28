#include "config.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "tvlight_protocol.h"

namespace tvlight {
namespace {
constexpr int kMaxSideLeds = 300;
constexpr int kMaxByte = 255;
constexpr int kMaxCurrentMa = 20000;
constexpr int kMinFps = 10;
constexpr int kMaxFps = 144;
constexpr int kMinCaptureFps = 5;
constexpr int kMinAudioBlock = 64;
constexpr int kMaxAudioBlock = 2048;
constexpr int kJsonIndent = 2;

std::string valid_or(const std::string& v, std::initializer_list<const char*> allowed, const char* fallback) {
  for (const char* a : allowed)
    if (v == a) return v;
  return fallback;
}

void sanitize(Config& c) {
  auto& l = c.layout;
  for (int* segment : {&l.skip, &l.bottom_left, &l.left, &l.top, &l.right, &l.bottom_right})
    *segment = std::clamp(*segment, 0, kMaxSideLeds);
  l.depth = std::clamp(l.depth, 0.02, 0.5);
  l.bottom_gap = std::clamp(l.bottom_gap, 0.0, 0.9);

  auto& b = c.bass;
  b.lowpass_hz = std::clamp(b.lowpass_hz, 20.0, 250.0);
  b.highpass_hz = std::clamp(b.highpass_hz, 5.0, b.lowpass_hz * 0.8);
  b.attack_ms = std::clamp(b.attack_ms, 0.5, 200.0);
  b.release_ms = std::clamp(b.release_ms, 10.0, 2000.0);
  b.agc_decay_s = std::clamp(b.agc_decay_s, 0.5, 60.0);
  b.sensitivity = std::clamp(b.sensitivity, 0.1, 100.0);
  b.threshold = std::clamp(b.threshold, 0.0, 0.95);
  b.curve = std::clamp(b.curve, 0.25, 4.0);
  b.share = std::clamp(b.share, 0.0, 1.0);
  b.smoothing_ms = std::clamp(b.smoothing_ms, 0.0, 1000.0);
  b.floor = std::clamp(b.floor, 0.0, 1.0);

  c.mode = valid_or(c.mode, {"ambient", "solid", "rainbow", "off"}, "ambient");
  c.capture_source = valid_or(c.capture_source, {"auto", "gamescope", "portal", "none"}, "auto");
  c.color_order = valid_or(c.color_order, {"RGB", "RBG", "GRB", "GBR", "BRG", "BGR"}, "GRB");
  c.led_count = std::clamp(c.led_count, 0, static_cast<int>(protocol::kMaxLeds));
  c.brightness = std::clamp(c.brightness, 0, kMaxByte);
  c.max_current_ma = std::clamp(c.max_current_ma, 0, kMaxCurrentMa);
  c.fps = std::clamp(c.fps, kMinFps, kMaxFps);
  c.audio_block = std::bit_ceil(static_cast<unsigned>(std::clamp(c.audio_block, kMinAudioBlock, kMaxAudioBlock)));
  c.capture_fps = std::clamp(c.capture_fps, kMinCaptureFps, kMaxFps);
  c.screen_gamma = std::clamp(c.screen_gamma, 1.0, 3.5);
  c.saturation = std::clamp(c.saturation, 0.0, 3.0);
  c.smoothing_ms = std::clamp(c.smoothing_ms, 0.0, 2000.0);
  for (double* w : {&c.white_r, &c.white_g, &c.white_b}) *w = std::clamp(*w, 0.0, 1.0);
}
}  // namespace


ConfigStore::ConfigStore(std::string path) : path_(std::move(path)) {}

void ConfigStore::load() {
  std::lock_guard lock(mutex_);
  std::ifstream in(path_);
  if (in) {
    try {
      config_ = nlohmann::json::parse(in).get<Config>();
      std::cerr << "config: loaded " << path_ << "\n";
    } catch (const std::exception& e) {
      std::cerr << "config: ignoring unreadable " << path_ << ": " << e.what() << "\n";
      config_ = Config{};
    }
  } else {
    std::cerr << "config: " << path_ << " not found, using defaults\n";
  }
  sanitize(config_);
  save_locked();
  ++version_;
}

Config ConfigStore::get() const {
  std::lock_guard lock(mutex_);
  return config_;
}

Config ConfigStore::patch(const nlohmann::json& patch) {
  std::lock_guard lock(mutex_);
  nlohmann::json j = config_;
  j.merge_patch(patch);
  Config next = j.get<Config>();
  sanitize(next);
  config_ = next;
  save_locked();
  ++version_;
  return config_;
}

void ConfigStore::set_restore_token(const std::string& token) {
  std::lock_guard lock(mutex_);
  if (config_.portal_restore_token == token) return;
  config_.portal_restore_token = token;
  save_locked();
  ++version_;
}

void ConfigStore::save_locked() const {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(fs::path(path_).parent_path(), ec);
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) {
      std::cerr << "config: cannot write " << tmp << "\n";
      return;
    }
    out << nlohmann::json(config_).dump(kJsonIndent) << "\n";
  }
  fs::rename(tmp, path_, ec);
  if (ec) std::cerr << "config: rename failed: " << ec.message() << "\n";
}
}  // namespace tvlight
