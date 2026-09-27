#include "ws2812.h"

#include "config.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include "ws2812.pio.h"

namespace tvlight::fw {
namespace {
constexpr unsigned kBitsPerLed = 24;
constexpr bool kShiftLeft = false;
constexpr bool kAutoPull = true;
}  // namespace

static PIO const kPio = pio0;

void Ws2812::init(unsigned pin) {
  sm_ = pio_claim_unused_sm(kPio, true);
  unsigned offset = pio_add_program(kPio, &ws2812_program);

  pio_gpio_init(kPio, pin);
  pio_sm_set_consecutive_pindirs(kPio, sm_, pin, 1, true);

  pio_sm_config c = ws2812_program_get_default_config(offset);
  sm_config_set_sideset_pins(&c, pin);
  sm_config_set_out_shift(&c, kShiftLeft, kAutoPull, kBitsPerLed);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
  constexpr int kCyclesPerBit = ws2812_T1 + ws2812_T2 + ws2812_T3;
  float div = static_cast<float>(clock_get_hz(clk_sys)) / (kWs2812BitHz * kCyclesPerBit);
  sm_config_set_clkdiv(&c, div);
  pio_sm_init(kPio, sm_, offset, &c);
  pio_sm_set_enabled(kPio, sm_, true);

  dma_channel_ = dma_claim_unused_channel(true);
  dma_channel_config dc = dma_channel_get_default_config(dma_channel_);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(kPio, sm_, true));
  dma_channel_configure(dma_channel_, &dc, &kPio->txf[sm_], nullptr, 0, false);
}

void Ws2812::transmit(const uint32_t* pixels, size_t count) {
  wait_done();
  if (count == 0) return;
  dma_channel_transfer_from_buffer_now(dma_channel_, pixels, count);
  busy_ = true;
}

void Ws2812::wait_done() {
  if (!busy_) return;
  dma_channel_wait_for_finish_blocking(dma_channel_);
  while (!pio_sm_is_tx_fifo_empty(kPio, sm_)) tight_loop_contents();
  // The last word is still shifting out of the OSR when the FIFO drains.
  sleep_us(kUsPerLed + kLatchUs);
  busy_ = false;
}

}  // namespace tvlight::fw
