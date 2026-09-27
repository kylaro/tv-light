#include "frame_store.h"

namespace tvlight::fw {

void FrameStore::init() { lock_ = spin_lock_init(spin_lock_claim_unused(true)); }

LinearFrame& FrameStore::begin_write() {
  irq_state_ = spin_lock_blocking(lock_);
  return pending_;
}

void FrameStore::publish() {
  seq_ = seq_ + 1;
  spin_unlock(lock_, irq_state_);
}

bool FrameStore::take(LinearFrame& out, uint32_t& seq) {
  if (seq_ == seq) return false;
  uint32_t irq = spin_lock_blocking(lock_);
  out = pending_;
  seq = seq_;
  spin_unlock(lock_, irq);
  return true;
}

}  // namespace tvlight::fw
