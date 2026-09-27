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

// Bottom LEDs run right -> left and skip a centered gap for the TV stand.
float bottom_x_from_right(float along, float gap) {
  const float half_run = (1.0f - gap) * kHalf;
  return along < half_run ? 1.0f - along : 1.0f - along - gap;
}
}  // namespace

void LedLayout::build(const LayoutConfig& cfg, int grid_w, int grid_h) {
  // Band depth in equal screen pixels on all sides, assuming the grid's aspect.
  const float depth_y = static_cast<float>(cfg.depth);
  const float depth_x = depth_y * static_cast<float>(grid_h) / static_cast<float>(grid_w);
  const float gap = static_cast<float>(cfg.bottom_gap);

  // Loop order (clockwise seen from the front): left up, top rightwards, right down, bottom leftwards.
  std::vector<std::pair<Rect, char>> loop;
  for (int i = 0; i < cfg.left; ++i) {
    const float a = static_cast<float>(i) / cfg.left, b = static_cast<float>(i + 1) / cfg.left;
    loop.push_back({{0.0f, 1.0f - b, depth_x, 1.0f - a}, 'l'});
  }
  for (int i = 0; i < cfg.top; ++i) {
    const float a = static_cast<float>(i) / cfg.top, b = static_cast<float>(i + 1) / cfg.top;
    loop.push_back({{a, 0.0f, b, depth_y}, 't'});
  }
  for (int i = 0; i < cfg.right; ++i) {
    const float a = static_cast<float>(i) / cfg.right, b = static_cast<float>(i + 1) / cfg.right;
    loop.push_back({{1.0f - depth_x, a, 1.0f, b}, 'r'});
  }
  const float run = 1.0f - gap;
  for (int i = 0; i < cfg.bottom; ++i) {
    const float a = run * i / cfg.bottom, b = run * (i + 1) / cfg.bottom;
    const float xa = bottom_x_from_right(a, gap), xb = bottom_x_from_right(b, gap);
    loop.push_back({{std::min(xa, xb), 1.0f - depth_y, std::max(xa, xb), 1.0f}, 'b'});
  }

  const int n = static_cast<int>(loop.size());
  if (n == 0) {
    zones_.assign(std::min<int>(cfg.skip, protocol::kMaxLeds), LedZone{});
    return;
  }
  const int total = std::min<int>(cfg.skip + n, protocol::kMaxLeds);
  zones_.assign(total, LedZone{});
  for (int k = cfg.skip; k < total; ++k) {
    const int strip = k - cfg.skip;
    const int step = cfg.direction == "ccw" ? -strip : strip;
    const int p = ((cfg.offset + step) % n + n) % n;
    const auto& [r, side] = loop[p];
    LedZone& z = zones_[k];
    z.side = side;
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
