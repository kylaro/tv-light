// Hand-off from core 0 (USB receiver) to core 1 (renderer).
#pragma once

#include <array>
#include <cstdint>

#include "config.h"
#include "hardware/sync.h"

namespace tvlight::fw {

// A decoded host frame with colors already converted to 16-bit linear light.
struct LinearFrame {
  uint16_t led_count = 0;
  uint8_t brightness = 0;
  uint8_t always_on_share = 0;
  uint8_t flags = 0;
  uint16_t max_current_ma = 0;
  uint16_t dark_leds = 0;
  std::array<uint16_t, protocol::kBytesPerLed> floor_lin{};
  std::array<uint16_t, kChannels> lin{};
};

// Frames: single-slot mailbox guarded by a hardware spin lock; the writer
// overwrites whatever the reader has not picked up, only the newest matters.
// Bass: one 32-bit word (sequence << 16 | value), a single atomic store on the M0+.
class FrameStore {
 public:
  void init();
  // Core 0: exclusive access to the pending slot; call publish() after filling it.
  LinearFrame& begin_write();
  void publish();
  // Core 1: copies the newest frame into `out` if one arrived since the last take.
  bool take(LinearFrame& out, uint32_t& seq);

  void post_bass(uint16_t bass) {
    constexpr unsigned kSeqShift = 16;
    bass_seq_ = static_cast<uint16_t>(bass_seq_ + 1);
    bass_word_ = (static_cast<uint32_t>(bass_seq_) << kSeqShift) | bass;
  }
  uint32_t bass_word() const { return bass_word_; }

 private:
  LinearFrame pending_;
  volatile uint32_t seq_ = 0;
  uint32_t irq_state_ = 0;
  spin_lock_t* lock_ = nullptr;
  uint16_t bass_seq_ = 0;
  volatile uint32_t bass_word_ = 0;
};

}  // namespace tvlight::fw
