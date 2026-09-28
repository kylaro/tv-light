// Maps strip LED indices to regions of the screen grid.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "config.h"

namespace tvlight {

// Strip segments in strip order: the skipped lead-in, then the loop starting at
// the bottom middle and running clockwise as seen from the front.
enum Segment : int { kSegSkip, kSegBottomLeft, kSegLeft, kSegTop, kSegRight, kSegBottomRight, kSegmentCount };
constexpr int kNoSegment = -1;
// Names match the LayoutConfig keys and the web API.
constexpr std::array<const char*, kSegmentCount> kSegmentNames = {"skip", "bottom_left", "left",
                                                                 "top",  "right",       "bottom_right"};
int segment_from_name(const std::string& name);  // kNoSegment if unknown

struct LedZone {
  std::vector<int> cells;  // indices into the video grid; empty for skipped LEDs
  float x = 0;             // zone center in normalized screen coords, for the web preview
  float y = 0;
  char side = '-';         // l, t, r, b, or '-' when skipped
  int segment = kSegSkip;
};

class LedLayout {
 public:
  void build(const LayoutConfig& cfg, int grid_w, int grid_h);
  const std::vector<LedZone>& zones() const { return zones_; }  // one per strip LED

 private:
  std::vector<LedZone> zones_;
};

}  // namespace tvlight
