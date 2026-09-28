#include "renderer.h"

#include <algorithm>

#include "pico/time.h"

namespace tvlight::fw {
namespace {
constexpr uint32_t kQ16One = 1u << 16;
constexpr uint32_t kMax16 = 0xFFFF;
constexpr uint32_t kMax8 = 0xFF;
constexpr unsigned kByteShift = 8;
constexpr unsigned kQ16Shift = 16;
constexpr uint32_t kUsPerMs = 1000;
constexpr unsigned kWireByteShift[protocol::kBytesPerLed] = {24, 16, 8};
// Interval EMA weight: new = old + (sample - old) / kIntervalSmoothing
constexpr int32_t kIntervalSmoothing = 8;

uint16_t lerp16(uint16_t a, uint16_t b, uint32_t t_q16) {
  int32_t d = static_cast<int32_t>(b) - static_cast<int32_t>(a);
  return static_cast<uint16_t>(a + ((static_cast<int64_t>(d) * t_q16) >> kQ16Shift));
}
}  // namespace

void Renderer::init(unsigned pin) { strip_.init(pin); }

void Renderer::pick_up_frame(uint64_t now_us) {
  if (!store_.take(incoming_, seq_)) return;
  const uint16_t previous_count = to_.led_count;

  if (have_frame_) {
    const int32_t interval = static_cast<int32_t>(now_us - last_frame_us_);
    const int32_t smoothed =
        static_cast<int32_t>(interp_len_us_) + (interval - static_cast<int32_t>(interp_len_us_)) / kIntervalSmoothing;
    interp_len_us_ = std::clamp<uint32_t>(static_cast<uint32_t>(std::max<int32_t>(smoothed, 0)), kMinInterpUs,
                                          kMaxInterpUs);
  }
  const bool interpolate = have_frame_ && (incoming_.flags & protocol::kFlagInterpolate) &&
                           incoming_.led_count == to_.led_count;
  // Start the new blend from whatever is on the strip right now.
  from_ = incoming_;
  if (interpolate) {
    from_.lin = shown_;
    from_.floor_lin = shown_floor_;
  }
  to_ = incoming_;
  interp_start_us_ = now_us;
  last_frame_us_ = now_us;
  have_frame_ = true;
  if (incoming_.led_count < transmit_count_) {
    // Arm once per shrink; re-arming every frame would never let it finish.
    if (incoming_.led_count != previous_count) blank_refreshes_left_ = kBlankRefreshes;
  } else {
    transmit_count_ = incoming_.led_count;
  }
}

void Renderer::pick_up_bass(uint64_t now_us) {
  constexpr uint32_t kValueMask = 0xFFFF;
  const uint32_t word = store_.bass_word();
  if (word == bass_word_) return;
  bass_word_ = word;
  if (last_bass_us_ != 0) {
    const int32_t interval = static_cast<int32_t>(now_us - last_bass_us_);
    const int32_t smoothed =
        static_cast<int32_t>(bass_interp_us_) + (interval - static_cast<int32_t>(bass_interp_us_)) / kIntervalSmoothing;
    bass_interp_us_ = std::clamp<uint32_t>(static_cast<uint32_t>(std::max<int32_t>(smoothed, 0)), kMinBassInterpUs,
                                           kMaxBassInterpUs);
  }
  bass_from_ = shown_bass_;
  bass_to_ = static_cast<uint16_t>(word & kValueMask);
  bass_start_us_ = now_us;
  last_bass_us_ = now_us;
}

uint16_t Renderer::current_bass(uint64_t now_us) {
  // Audio went quiet (sink suspended, daemon gone): let the pump fall to zero.
  if (now_us - last_bass_us_ > kBassTimeoutUs && bass_to_ != 0) {
    bass_from_ = shown_bass_;
    bass_to_ = 0;
    bass_start_us_ = now_us;
    bass_interp_us_ = kMaxBassInterpUs;
  }
  const uint64_t elapsed = now_us - bass_start_us_;
  const uint32_t t_q16 =
      elapsed >= bass_interp_us_ ? kQ16One : static_cast<uint32_t>((elapsed << kQ16Shift) / bass_interp_us_);
  return lerp16(bass_from_, bass_to_, t_q16);
}

void Renderer::render(uint64_t now_us, uint32_t* out_words) {
  const LinearFrame& f = to_;
  const size_t channels = static_cast<size_t>(f.led_count) * protocol::kBytesPerLed;

  uint32_t t_q16 = kQ16One;
  if (f.flags & protocol::kFlagInterpolate) {
    const uint64_t elapsed = now_us - interp_start_us_;
    t_q16 = elapsed >= interp_len_us_ ? kQ16One : static_cast<uint32_t>((elapsed << kQ16Shift) / interp_len_us_);
  }

  // Fade out when the host goes quiet (app closed, cable pulled, ...).
  uint32_t fade_q16 = kQ16One;
  const uint64_t age_us = now_us - last_frame_us_;
  constexpr uint64_t kTimeoutUs = static_cast<uint64_t>(kFrameTimeoutMs) * kUsPerMs;
  constexpr uint64_t kFadeUs = static_cast<uint64_t>(kFadeOutMs) * kUsPerMs;
  if (!have_frame_ || age_us >= kTimeoutUs + kFadeUs) {
    fade_q16 = 0;
  } else if (age_us > kTimeoutUs) {
    fade_q16 = kQ16One - static_cast<uint32_t>(((age_us - kTimeoutUs) << kQ16Shift) / kFadeUs);
  }

  shown_bass_ = current_bass(now_us);
  for (size_t c = 0; c < protocol::kBytesPerLed; ++c) shown_floor_[c] = lerp16(from_.floor_lin[c], f.floor_lin[c], t_q16);

  // out = brightness * fade * (base * (always_on + (1 - always_on) * bass) + floor * bass)
  const uint32_t bass = shown_bass_;
  const uint32_t always_on_q16 = (static_cast<uint32_t>(f.always_on_share) << kQ16Shift) / protocol::kShareOne;
  const uint32_t pump_q16 = always_on_q16 + (((kQ16One - always_on_q16) * static_cast<uint64_t>(bass)) >> kQ16Shift);
  const uint32_t bright_q8 = static_cast<uint32_t>(f.brightness) + 1;  // 256 == full
  const uint64_t scale_q24 = static_cast<uint64_t>(bright_q8) * fade_q16;  // Q8 * Q16
  uint32_t floor_term[protocol::kBytesPerLed];
  for (size_t c = 0; c < protocol::kBytesPerLed; ++c) floor_term[c] = (shown_floor_[c] * bass) >> kQ16Shift;

  // Skipped leading LEDs stay off: the host sends them black, but the floor would still pump them.
  const size_t dark_channels = static_cast<size_t>(f.dark_leds) * protocol::kBytesPerLed;
  uint64_t channel_sum = 0;
  for (size_t i = 0; i < channels; ++i) {
    const uint16_t base = lerp16(from_.lin[i], f.lin[i], t_q16);
    shown_[i] = base;
    const uint32_t floor = i < dark_channels ? 0 : floor_term[i % protocol::kBytesPerLed];
    const uint64_t pumped = ((static_cast<uint64_t>(base) * pump_q16) >> kQ16Shift) + floor;
    const uint64_t v = (pumped * scale_q24) >> (kByteShift + kQ16Shift);
    const uint32_t clamped = static_cast<uint32_t>(std::min<uint64_t>(v, kMax16));
    mixed_[i] = clamped;
    channel_sum += clamped;
  }

  // Current limiter: scale everything down uniformly if over budget.
  const uint32_t idle_ma = (static_cast<uint32_t>(f.led_count) * kIdleMicroampsPerLed) / kUsPerMs;
  const uint32_t color_ma = static_cast<uint32_t>((channel_sum * kMilliampsPerChannel) / kMax16);
  uint32_t limit_q16 = kQ16One;
  if (f.max_current_ma > 0 && color_ma + idle_ma > f.max_current_ma) {
    const uint32_t budget = f.max_current_ma > idle_ma ? f.max_current_ma - idle_ma : 0;
    limit_q16 = static_cast<uint32_t>((static_cast<uint64_t>(budget) << kQ16Shift) / color_ma);
    limited_frames = limited_frames + 1;
    last_milliamps = f.max_current_ma;
  } else {
    last_milliamps = color_ma + idle_ma;
  }

  const bool dither = f.flags & protocol::kFlagDither;
  for (size_t led = 0; led < transmit_count_; ++led) {
    uint32_t word = 0;
    for (size_t c = 0; c < protocol::kBytesPerLed; ++c) {
      const size_t i = led * protocol::kBytesPerLed + c;
      uint32_t out8 = 0;
      if (led < f.led_count) {
        uint32_t v16 = static_cast<uint32_t>((static_cast<uint64_t>(mixed_[i]) * limit_q16) >> kQ16Shift);
        if (dither) {
          // Temporal dithering: carry the dropped low byte into the next refresh.
          const uint32_t with_err = std::min<uint32_t>(v16 + dither_err_[i], kMax16);
          out8 = with_err >> kByteShift;
          dither_err_[i] = static_cast<uint8_t>(with_err & kMax8);
        } else {
          out8 = (v16 + (kMax8 >> 1)) >> kByteShift;
          out8 = std::min(out8, kMax8);
        }
      }
      word |= out8 << kWireByteShift[c];
    }
    out_words[led] = word;
  }
}

void Renderer::run() {
  int back = 0;
  uint64_t last_render_us = 0;
  while (true) {
    uint64_t now = time_us_64();
    if (now - last_render_us < kMinRenderIntervalUs) {
      sleep_us(static_cast<uint32_t>(kMinRenderIntervalUs - (now - last_render_us)));
      now = time_us_64();
    }
    last_render_us = now;
    pick_up_frame(now);
    pick_up_bass(now);
    render(now, words_[back].data());
    strip_.transmit(words_[back].data(), transmit_count_);  // waits for previous transfer + latch
    back ^= 1;
    renders = renders + 1;
    if (blank_refreshes_left_ > 0 && --blank_refreshes_left_ == 0) transmit_count_ = to_.led_count;
  }
}

}  // namespace tvlight::fw
