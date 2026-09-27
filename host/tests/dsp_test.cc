// Checks the bass chain responds to low tones and rejects higher ones.
#include <cmath>
#include <cstdio>
#include <numbers>

#include "dsp.h"

namespace {
constexpr double kFs = 48000.0;
constexpr double kSeconds = 1.0;

double settled_envelope(double freq_hz) {
  using namespace tvlight::dsp;
  Biquad hp = Biquad::highpass(kFs, 20.0, kButterworth2Q);
  Biquad lp1 = Biquad::lowpass(kFs, 50.0, kButterworth4Q1);
  Biquad lp2 = Biquad::lowpass(kFs, 50.0, kButterworth4Q2);
  EnvelopeFollower env;
  env.set(0.005, 0.150, kFs);
  float peak = 0;
  const int n = static_cast<int>(kFs * kSeconds);
  for (int i = 0; i < n; ++i) {
    const float x = static_cast<float>(std::sin(2.0 * std::numbers::pi * freq_hz * i / kFs));
    const float e = env.process(lp2.process(lp1.process(hp.process(x))));
    if (i > n / 2) peak = std::max(peak, e);
  }
  return peak;
}
}  // namespace

int main() {
  int failures = 0;
  struct Case { double hz; double min; double max; };
  const Case cases[] = {{10, 0.0, 0.35}, {40, 0.7, 1.05}, {50, 0.6, 0.8}, {200, 0.0, 0.03}, {1000, 0.0, 0.001}};
  for (const Case& c : cases) {
    const double e = settled_envelope(c.hz);
    const bool ok = e >= c.min && e <= c.max;
    std::printf("%6.0f Hz -> envelope %.4f %s\n", c.hz, e, ok ? "ok" : "FAIL");
    failures += !ok;
  }
  return failures == 0 ? 0 : 1;
}
