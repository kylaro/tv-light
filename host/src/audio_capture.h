// Captures what is going to the speakers (default sink monitor) and turns
// the sub-bass into a 0..1 envelope for pumping the LEDs.
#pragma once

#include <spa/utils/hook.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "config.h"
#include "dsp.h"
#include "pw_loop.h"

struct pw_stream;

namespace tvlight {

struct AudioStatus {
  std::string state = "idle";
  float bass = 0;       // shaped 0..1 output
  float envelope = 0;   // raw low-passed envelope (full scale = 1)
  float reference = 0;  // AGC peak (or 1/sensitivity) the envelope is divided by
};

class AudioCapture {
 public:
  AudioCapture(PwLoop& pw, ConfigStore& config);
  ~AudioCapture();
  // Called on the PipeWire thread with each new bass value (once per audio block).
  void on_bass(std::function<void(float)> cb) { bass_cb_ = std::move(cb); }
  void start();
  // Picks up config changes; call from the main loop.
  void tick();
  // Returns 0 when the sink has gone quiet/suspended and no buffers arrive.
  float bass() const;
  AudioStatus status() const;

 private:
  static void on_process(void* data);
  static void on_state(void* data, int old_state, int state, const char* error);
  void rebuild_filters(const BassConfig& cfg);

  PwLoop& pw_;
  ConfigStore& config_;
  pw_stream* stream_ = nullptr;
  spa_hook listener_{};

  // Touched only on the PipeWire thread (or with the loop locked).
  BassConfig cfg_;
  uint64_t cfg_version_ = 0;
  int audio_block_ = 0;
  std::function<void(float)> bass_cb_;
  dsp::Biquad highpass_;
  dsp::Biquad lowpass1_;
  dsp::Biquad lowpass2_;
  dsp::EnvelopeFollower envelope_;
  dsp::PeakTracker peak_;
  float smoothed_ = 0;  // EMA of the shaped bass value

  std::atomic<float> bass_{0};
  std::atomic<float> envelope_value_{0};
  std::atomic<float> reference_{0};
  std::atomic<int64_t> last_update_ns_{0};
  mutable std::mutex mutex_;
  std::string state_ = "idle";
};

}  // namespace tvlight
