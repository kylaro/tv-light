#pragma once

#include <cstddef>
#include <cstdint>

namespace tvlight::fw {

// PIO + DMA WS2812 output. Pixels are 32-bit words, first wire byte in bits 31..24.
class Ws2812 {
 public:
  void init(unsigned pin);
  // Starts a DMA transfer; `pixels` must stay valid until wait_done() returns.
  void transmit(const uint32_t* pixels, size_t count);
  // Blocks until the last transfer is fully shifted out and the latch time passed.
  void wait_done();

 private:
  unsigned sm_ = 0;
  int dma_channel_ = -1;
  bool busy_ = false;
};

}  // namespace tvlight::fw
