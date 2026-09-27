// Maps strip LED indices to regions of the screen grid.
#pragma once

#include <vector>

#include "config.h"

namespace tvlight {

struct LedZone {
  std::vector<int> cells;  // indices into the video grid; empty for skipped LEDs
  float x = 0;             // zone center in normalized screen coords, for the web preview
  float y = 0;
  char side = '-';         // l, t, r, b, or '-' when skipped
};

class LedLayout {
 public:
  void build(const LayoutConfig& cfg, int grid_w, int grid_h);
  const std::vector<LedZone>& zones() const { return zones_; }  // one per strip LED

 private:
  std::vector<LedZone> zones_;
};

}  // namespace tvlight
