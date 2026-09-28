#include "led_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "tvlight_protocol.h"

namespace tvlight {
namespace {
struct Rect {
  float x0, y0, x1, y1;
};

constexpr float kHalf = 0.5f;
constexpr std::array<char, kSegmentCount> kSegmentSide = {'-', 'b', 'l', 't', 'r', 'b'};
}  // namespace

int segment_from_name(const std::string& name) {
  for (int s = 0; s < kSegmentCount; ++s)
    if (name == kSegmentNames[s]) return s;
  return kNoSegment;
}

void LedLayout::build(const LayoutConfig& cfg, int grid_w, int grid_h) {
  // Band depth in equal screen pixels on all sides, assuming the grid's aspect.
  const float depth_y = static_cast<float>(cfg.depth);
  const float depth_x = depth_y * static_cast<float>(grid_h) / static_cast<float>(grid_w);
  // Each bottom half spans from its corner to the centered stand gap.
  const float half_run = (1.0f - static_cast<float>(cfg.bottom_gap)) * kHalf;

  // Mapped LEDs in strip order; `a`..`b` is the LED's fraction along its segment.
  std::vector<std::pair<Rect, int>> strip;
  auto add = [&](int count, int segment, auto rect_at) {
    for (int i = 0; i < count; ++i) {
      const float a = static_cast<float>(i) / count, b = static_cast<float>(i + 1) / count;
      strip.push_back({rect_at(a, b), segment});
    }
  };
  add(cfg.bottom_left, kSegBottomLeft,
      [&](float a, float b) { return Rect{half_run * (1.0f - b), 1.0f - depth_y, half_run * (1.0f - a), 1.0f}; });
  add(cfg.left, kSegLeft, [&](float a, float b) { return Rect{0.0f, 1.0f - b, depth_x, 1.0f - a}; });
  add(cfg.top, kSegTop, [&](float a, float b) { return Rect{a, 0.0f, b, depth_y}; });
  add(cfg.right, kSegRight, [&](float a, float b) { return Rect{1.0f - depth_x, a, 1.0f, b}; });
  add(cfg.bottom_right, kSegBottomRight,
      [&](float a, float b) { return Rect{1.0f - half_run * b, 1.0f - depth_y, 1.0f - half_run * a, 1.0f}; });

  const int skip = std::min<int>(cfg.skip, protocol::kMaxLeds);
  const int total = std::min<int>(skip + static_cast<int>(strip.size()), protocol::kMaxLeds);
  zones_.assign(total, LedZone{});
  for (int k = skip; k < total; ++k) {
    const auto& [r, segment] = strip[k - skip];
    LedZone& z = zones_[k];
    z.segment = segment;
    z.side = kSegmentSide[segment];
    z.x = (r.x0 + r.x1) * kHalf;
    z.y = (r.y0 + r.y1) * kHalf;
    int nearest = 0;
    float nearest_d = std::numeric_limits<float>::max();
    for (int gy = 0; gy < grid_h; ++gy) {
      for (int gx = 0; gx < grid_w; ++gx) {
        const float cx = (gx + kHalf) / grid_w, cy = (gy + kHalf) / grid_h;
        if (cx >= r.x0 && cx <= r.x1 && cy >= r.y0 && cy <= r.y1) z.cells.push_back(gy * grid_w + gx);
        const float d = std::hypot(cx - z.x, cy - z.y);
        if (d < nearest_d) {
          nearest_d = d;
          nearest = gy * grid_w + gx;
        }
      }
    }
    if (z.cells.empty()) z.cells.push_back(nearest);
  }
}

}  // namespace tvlight
