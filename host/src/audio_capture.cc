#include "audio_capture.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <chrono>
#include <cmath>
#include <iostream>

namespace tvlight {
namespace {
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 1;  // PipeWire downmixes the sink monitor for us
constexpr size_t kPodBufferSize = 1024;
constexpr double kMsPerSecond = 1000.0;

std::string node_latency(int block) { return std::to_string(block) + "/" + std::to_string(kSampleRate); }
constexpr int64_t kStaleNs = 200'000'000;  // no audio buffers for 200 ms -> treat as silence

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

AudioCapture::AudioCapture(PwLoop& pw, ConfigStore& config) : pw_(pw), config_(config) {}

AudioCapture::~AudioCapture() {
  PwLoop::Lock lock(pw_);
  if (stream_) {
    spa_hook_remove(&listener_);
    pw_stream_destroy(stream_);
  }
}

void AudioCapture::rebuild_filters(const BassConfig& cfg) {
  cfg_ = cfg;
  highpass_ = dsp::Biquad::highpass(kSampleRate, cfg.highpass_hz, dsp::kButterworth2Q);
  lowpass1_ = dsp::Biquad::lowpass(kSampleRate, cfg.lowpass_hz, dsp::kButterworth4Q1);
  lowpass2_ = dsp::Biquad::lowpass(kSampleRate, cfg.lowpass_hz, dsp::kButterworth4Q2);
  envelope_.set(cfg.attack_ms / kMsPerSecond, cfg.release_ms / kMsPerSecond, kSampleRate);
  peak_.set(cfg.agc_decay_s, kSampleRate);
}

void AudioCapture::start() {
  static pw_stream_events events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = &AudioCapture::on_process;
    e.state_changed = reinterpret_cast<void (*)(void*, pw_stream_state, pw_stream_state, const char*)>(
        &AudioCapture::on_state);
    return e;
  }();

  PwLoop::Lock lock(pw_);
  const Config cfg = config_.get();
  rebuild_filters(cfg.bass);
  cfg_version_ = config_.version();
  audio_block_ = cfg.audio_block;

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music",
      PW_KEY_STREAM_CAPTURE_SINK, "true",  // record the default sink's monitor, not a mic
      PW_KEY_NODE_LATENCY, node_latency(audio_block_).c_str(), PW_KEY_NODE_NAME, "tvlight-bass", PW_KEY_APP_NAME, "TV Light", nullptr);
  stream_ = pw_stream_new(pw_.core(), "tvlight-bass", props);
  pw_stream_add_listener(stream_, &listener_, &events, this);

  uint8_t buffer[kPodBufferSize];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = kSampleRate;
  info.channels = kChannels;
  const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
  const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
  pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1);
}

void AudioCapture::tick() {
  const uint64_t v = config_.version();
  if (v == cfg_version_) return;
  const Config cfg = config_.get();
  PwLoop::Lock lock(pw_);
  cfg_version_ = v;
  rebuild_filters(cfg.bass);
  if (stream_ && cfg.audio_block != audio_block_) {
    audio_block_ = cfg.audio_block;
    const std::string latency = node_latency(audio_block_);
    const spa_dict_item items[] = {{PW_KEY_NODE_LATENCY, latency.c_str()}};
    const spa_dict dict = SPA_DICT_INIT_ARRAY(items);
    pw_stream_update_properties(stream_, &dict);
    std::cerr << "audio: block now " << latency << "\n";
  }
}

void AudioCapture::on_state(void* data, int, int state, const char* error) {
  auto* self = static_cast<AudioCapture*>(data);
  const auto s = static_cast<pw_stream_state>(state);
  std::cerr << "audio: stream " << pw_stream_state_as_string(s) << (error ? std::string(": ") + error : "") << "\n";
  std::lock_guard lock(self->mutex_);
  self->state_ = error ? std::string("error: ") + error : pw_stream_state_as_string(s);
}

void AudioCapture::on_process(void* data) {
  auto* self = static_cast<AudioCapture*>(data);
  pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
  if (!b) return;
  const spa_data& d = b->buffer->datas[0];
  if (d.data && d.chunk) {
    const auto* samples = reinterpret_cast<const float*>(static_cast<const uint8_t*>(d.data) + d.chunk->offset);
    const size_t n = d.chunk->size / sizeof(float);
    const BassConfig& cfg = self->cfg_;
    float env = 0;
    float ref = 1.0f;
    for (size_t i = 0; i < n; ++i) {
      const float filtered = self->lowpass2_.process(self->lowpass1_.process(self->highpass_.process(samples[i])));
      env = self->envelope_.process(filtered);
      ref = self->peak_.process(env);
    }
    if (n > 0) {
      if (!cfg.agc) ref = static_cast<float>(1.0 / cfg.sensitivity);
      const float shaped = cfg.enabled ? dsp::shape(env / ref, cfg.threshold, cfg.curve) : 0.0f;
      // EMA across blocks: alpha from the block duration and the smoothing time constant.
      const double block_s = static_cast<double>(n) / kSampleRate;
      const double alpha = cfg.smoothing_ms > 0 ? 1.0 - std::exp(-block_s * kMsPerSecond / cfg.smoothing_ms) : 1.0;
      self->smoothed_ += static_cast<float>(alpha) * (shaped - self->smoothed_);
      const float out = self->smoothed_;
      self->bass_ = out;
      self->envelope_value_ = env;
      self->reference_ = ref;
      self->last_update_ns_ = now_ns();
      if (self->bass_cb_) self->bass_cb_(out);
    }
  }
  pw_stream_queue_buffer(self->stream_, b);
}

float AudioCapture::bass() const { return now_ns() - last_update_ns_ > kStaleNs ? 0.0f : bass_.load(); }

AudioStatus AudioCapture::status() const {
  AudioStatus s;
  {
    std::lock_guard lock(mutex_);
    s.state = state_;
  }
  s.bass = bass();
  s.envelope = envelope_value_;
  s.reference = reference_;
  return s;
}

}  // namespace tvlight
