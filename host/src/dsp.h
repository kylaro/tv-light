// Small audio DSP blocks for the bass follower. Header only so it can be unit tested.
#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tvlight::dsp {

// RBJ cookbook biquad, transposed direct form II.
class Biquad {
 public:
  static Biquad lowpass(double fs, double f, double q) { return make(fs, f, q, true); }
  static Biquad highpass(double fs, double f, double q) { return make(fs, f, q, false); }

  float process(float x) {
    const double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return static_cast<float>(y);
  }

 private:
  static Biquad make(double fs, double f, double q, bool low) {
    const double w0 = 2.0 * std::numbers::pi * f / fs;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Biquad bq;
    if (low) {
      bq.b0_ = (1.0 - cosw) / 2.0 / a0;
      bq.b1_ = (1.0 - cosw) / a0;
    } else {
      bq.b0_ = (1.0 + cosw) / 2.0 / a0;
      bq.b1_ = -(1.0 + cosw) / a0;
    }
    bq.b2_ = bq.b0_;
    bq.a1_ = -2.0 * cosw / a0;
    bq.a2_ = (1.0 - alpha) / a0;
    return bq;
  }

  double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  double z1_ = 0, z2_ = 0;
};

// Q values of the two sections of a 4th order Butterworth filter.
constexpr double kButterworth4Q1 = 0.54119610;
constexpr double kButterworth4Q2 = 1.30656296;
constexpr double kButterworth2Q = std::numbers::sqrt2 / 2.0;

// One-pole coefficient that reaches 1 - 1/e of a step after `seconds`.
inline double time_constant_coeff(double seconds, double fs) { return std::exp(-1.0 / (seconds * fs)); }

// Peak envelope follower with separate attack and release.
class EnvelopeFollower {
 public:
  void set(double attack_s, double release_s, double fs) {
    attack_ = time_constant_coeff(attack_s, fs);
    release_ = time_constant_coeff(release_s, fs);
  }
  float process(float x) {
    const double rect = std::fabs(x);
    const double a = rect > env_ ? attack_ : release_;
    env_ = a * env_ + (1.0 - a) * rect;
    return static_cast<float>(env_);
  }
  float value() const { return static_cast<float>(env_); }

 private:
  double attack_ = 0, release_ = 0, env_ = 0;
};

// Tracks a slowly decaying peak so the envelope can be normalized to ~0..1
// regardless of playback volume. The floor keeps silence from being amplified.
class PeakTracker {
 public:
  static constexpr double kMinPeak = 0.01;  // about -40 dBFS
  void set(double decay_s, double fs) { decay_ = time_constant_coeff(decay_s, fs); }
  float process(float x) {
    peak_ = std::max<double>(x, peak_ * decay_);
    return static_cast<float>(std::max(peak_, kMinPeak));
  }

 private:
  double decay_ = 0, peak_ = kMinPeak;
};

// Maps a normalized envelope to the 0..1 pump value: threshold, then curve.
inline float shape(float normalized, double threshold, double curve) {
  const double x = std::clamp((normalized - threshold) / (1.0 - threshold), 0.0, 1.0);
  return static_cast<float>(std::pow(x, curve));
}

}  // namespace tvlight::dsp
