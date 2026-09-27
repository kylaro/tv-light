#include "compositor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "tvlight_protocol.h"

namespace tvlight {
namespace {
constexpr double kWireGamma = protocol::kGammaTenthsDefault / 10.0;
constexpr float kMaxByte = 255.0f;
constexpr float kHalf = 0.5f;
constexpr float kDarkFloorThreshold = 1e-3f;  // scene darker than this: floor pumps white
constexpr double kRainbowCyclesPerSecond = 0.05;
constexpr double kFpsWindowSeconds = 1.0;
// Rec. 709 luma weights, for saturation in linear light.
constexpr Rgb kLuma = {0.2126f, 0.7152f, 0.0722f};
constexpr int kHexBase = 16;
constexpr unsigned kRedShift = 16;
constexpr unsigned kGreenShift = 8;
constexpr uint32_t kByteMask = 0xFF;
constexpr int kHueSectors = 6;

uint8_t encode(float lin, double gamma) {
  const double v = std::pow(std::clamp(lin, 0.0f, 1.0f), 1.0 / gamma) * kMaxByte + kHalf;
  return static_cast<uint8_t>(std::min<double>(v, kMaxByte));
}

float decode(uint8_t v, double gamma) { return static_cast<float>(std::pow(v / kMaxByte, gamma)); }

uint32_t pack_display(const Rgb& c) {
  return (static_cast<uint32_t>(encode(c[0], kWireGamma)) << kRedShift) |
         (static_cast<uint32_t>(encode(c[1], kWireGamma)) << kGreenShift) | encode(c[2], kWireGamma);
}

Rgb parse_hex_color(const std::string& s, double gamma) {
  constexpr size_t kHexColorLength = 7;  // "#rrggbb"
  if (s.size() != kHexColorLength || s[0] != '#') return {0, 0, 0};
  uint32_t v = 0;
  try {
    v = static_cast<uint32_t>(std::stoul(s.substr(1), nullptr, kHexBase));
  } catch (const std::exception&) {
    return {0, 0, 0};
  }
  return {decode((v >> kRedShift) & kByteMask, gamma), decode((v >> kGreenShift) & kByteMask, gamma),
          decode(v & kByteMask, gamma)};
}

// HSV with h in [0,1), full saturation/value, returned as gamma-encoded 0..1 values.
Rgb hue_to_rgb(double h) {
  const double x = h * kHueSectors;
  const int sector = static_cast<int>(x) % kHueSectors;
  const float f = static_cast<float>(x - std::floor(x));
  switch (sector) {
    case 0: return {1, f, 0};
    case 1: return {1 - f, 1, 0};
    case 2: return {0, 1, f};
    case 3: return {0, 1 - f, 1};
    case 4: return {f, 0, 1};
    default: return {1, 0, 1 - f};
  }
}
}  // namespace

Compositor::Compositor(ConfigStore& config, VideoCapture& video, AudioCapture& audio, SerialLink& serial)
    : config_(config), video_(video), audio_(audio), serial_(serial) {}

void Compositor::base_colors(const Config& cfg, std::vector<Rgb>& out) {
  const auto& zones = layout_.zones();
  out.assign(zones.size(), Rgb{0, 0, 0});
  if (cfg.mode == "off") return;

  if (cfg.mode == "solid") {
    const Rgb c = parse_hex_color(cfg.solid_color, kWireGamma);
    for (size_t i = 0; i < zones.size(); ++i)
      if (!zones[i].cells.empty()) out[i] = c;
    return;
  }
  if (cfg.mode == "rainbow") {
    const double shift = time_s_ * kRainbowCyclesPerSecond;
    for (size_t i = 0; i < zones.size(); ++i) {
      if (zones[i].cells.empty()) continue;
      const double h = std::fmod(static_cast<double>(i) / zones.size() + shift, 1.0);
      const Rgb e = hue_to_rgb(h);
      for (int c = 0; c < kRgb; ++c) out[i][c] = static_cast<float>(std::pow(e[c], kWireGamma));
    }
    return;
  }

  // Ambient: average each LED's screen zone.
  video_.latest(grid_);
  const float sat = static_cast<float>(cfg.saturation);
  for (size_t i = 0; i < zones.size(); ++i) {
    const auto& cells = zones[i].cells;
    if (cells.empty()) continue;
    Rgb sum{0, 0, 0};
    for (int cell : cells)
      for (int c = 0; c < kRgb; ++c) sum[c] += grid_.lin[cell * kRgb + c];
    Rgb avg;
    for (int c = 0; c < kRgb; ++c) avg[c] = sum[c] / cells.size();
    const float luma = avg[0] * kLuma[0] + avg[1] * kLuma[1] + avg[2] * kLuma[2];
    for (int c = 0; c < kRgb; ++c) out[i][c] = std::clamp(luma + (avg[c] - luma) * sat, 0.0f, 1.0f);
  }
}

void Compositor::step(double dt) {
  const Config cfg = config_.get();
  time_s_ += dt;
  bool layout_changed = false;
  if (config_.version() != layout_version_) {
    layout_version_ = config_.version();
    layout_.build(cfg.layout, kGridW, kGridH);
    layout_changed = true;
  }

  base_colors(cfg, target_);
  const Rgb white_balance = {static_cast<float>(cfg.white_r), static_cast<float>(cfg.white_g),
                             static_cast<float>(cfg.white_b)};
  for (Rgb& c : target_)
    for (int k = 0; k < kRgb; ++k) c[k] *= white_balance[k];

  // Exponential smoothing; test modes snap immediately.
  if (smoothed_.size() != target_.size()) smoothed_ = target_;
  const bool smooth = cfg.mode == "ambient" && cfg.smoothing_ms > 0;
  const float alpha = smooth ? static_cast<float>(1.0 - std::exp(-dt * 1000.0 / cfg.smoothing_ms)) : 1.0f;
  for (size_t i = 0; i < target_.size(); ++i)
    for (int k = 0; k < kRgb; ++k) smoothed_[i][k] += (target_[i][k] - smoothed_[i][k]) * alpha;

  // Floor color for the bass pump: the scene's average hue at full value.
  Rgb avg{0, 0, 0};
  for (const Rgb& c : smoothed_)
    for (int k = 0; k < kRgb; ++k) avg[k] += c[k];
  const float peak = std::max({avg[0], avg[1], avg[2]});
  Rgb floor = peak > kDarkFloorThreshold * std::max<size_t>(1, smoothed_.size())
                  ? Rgb{avg[0] / peak, avg[1] / peak, avg[2] / peak}
                  : white_balance;
  for (float& f : floor) f *= static_cast<float>(cfg.bass.floor);

  const float bass = cfg.mode == "off" ? 0.0f : audio_.bass();
  serial_.post_frame(encode_packet(cfg, floor));

  // Web preview snapshot.
  ++frames_;
  ++fps_window_frames_;
  fps_window_s_ += dt;
  std::lock_guard lock(mutex_);
  if (fps_window_s_ >= kFpsWindowSeconds) {
    snapshot_.fps = fps_window_frames_ / fps_window_s_;
    fps_window_s_ = 0;
    fps_window_frames_ = 0;
  }
  snapshot_.leds.resize(smoothed_.size());
  for (size_t i = 0; i < smoothed_.size(); ++i) snapshot_.leds[i] = pack_display(smoothed_[i]);
  if (layout_changed) snapshot_.zones = layout_.zones();
  snapshot_.floor = pack_display(floor);
  snapshot_.bass = bass;
  snapshot_.thumbnail.resize(static_cast<size_t>(kGridW) * kGridH * kRgb);
  for (size_t i = 0; i < snapshot_.thumbnail.size(); ++i) snapshot_.thumbnail[i] = encode(grid_.lin[i], cfg.screen_gamma);
}

std::vector<uint8_t> Compositor::encode_packet(const Config& cfg, const Rgb& floor) {
  // Display RGB -> wire byte order, e.g. "GRB" puts green first.
  std::array<int, kRgb> order{};
  for (int k = 0; k < kRgb; ++k) order[k] = static_cast<int>(std::string("RGB").find(cfg.color_order[k]));

  const auto count = static_cast<uint16_t>(smoothed_.size());
  std::vector<uint8_t> packet(protocol::frame_packet_size(count));
  protocol::FrameHeader h{};
  std::memcpy(h.prefix.magic, protocol::kMagic, sizeof(h.prefix.magic));
  h.prefix.version = protocol::kVersion;
  h.prefix.kind = protocol::kKindFrame;
  h.led_count = count;
  h.brightness = static_cast<uint8_t>(cfg.mode == "off" ? 0 : cfg.brightness);
  h.always_on_share = static_cast<uint8_t>(std::lround((1.0 - cfg.bass.share) * protocol::kShareOne));
  for (int k = 0; k < kRgb; ++k) h.floor_color[k] = encode(floor[order[k]], kWireGamma);
  h.flags = (cfg.dither ? protocol::kFlagDither : 0) | (cfg.interpolate ? protocol::kFlagInterpolate : 0);
  h.gamma_tenths = protocol::kGammaTenthsDefault;
  h.max_current_ma = static_cast<uint16_t>(cfg.max_current_ma);
  std::memcpy(packet.data(), &h, sizeof(h));

  uint8_t* px = packet.data() + sizeof(h);
  for (const Rgb& c : smoothed_)
    for (int k = 0; k < kRgb; ++k) *px++ = encode(c[order[k]], kWireGamma);

  protocol::seal(packet.data(), packet.size());
  return packet;
}

Snapshot Compositor::snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

}  // namespace tvlight
