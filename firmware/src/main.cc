// TV Light firmware for the FireFly V1.1: USB CDC packets from tvlightd -> WS2812B strip.
// Core 0 receives and decodes packets, core 1 renders.
#include <cstdio>

#include "config.h"
#include "frame_receiver.h"
#include "frame_store.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "renderer.h"

namespace {
using namespace tvlight::fw;
constexpr uint32_t kPollTimeoutUs = 2000;
constexpr uint32_t kPullSettleUs = 10;

FrameStore g_store;
Renderer g_renderer(g_store);

void core1_main() { g_renderer.run(); }

// BOARD_ID_0..4 straps, read with pull-ups; never driven.
uint32_t read_board_id() {
  for (unsigned i = 0; i < kBoardIdBits; ++i) {
    gpio_init(kBoardIdFirstPin + i);
    gpio_set_dir(kBoardIdFirstPin + i, GPIO_IN);
    gpio_pull_up(kBoardIdFirstPin + i);
  }
  sleep_us(kPullSettleUs);
  uint32_t id = 0;
  for (unsigned i = 0; i < kBoardIdBits; ++i) id |= (gpio_get(kBoardIdFirstPin + i) ? 1u : 0u) << i;
  // Leave them floating-safe: inputs without pulls so a strap to GND draws nothing.
  for (unsigned i = 0; i < kBoardIdBits; ++i) gpio_disable_pulls(kBoardIdFirstPin + i);
  return id;
}
}  // namespace

int main() {
  stdio_init_all();
  const uint32_t board_id = read_board_id();

  gpio_init(kStatusLedPin);
  gpio_set_dir(kStatusLedPin, GPIO_OUT);
  bool status_led = false;

  g_store.init();
  g_renderer.init(kLedDataPin);
  multicore_launch_core1(core1_main);

  FrameReceiver receiver(g_store);
  absolute_time_t next_status = make_timeout_time_ms(kStatusIntervalMs);
  absolute_time_t next_heartbeat = make_timeout_time_ms(kHeartbeatMs);
  uint32_t renders_at_last_status = 0;
  uint32_t frames_at_last_status = 0;
  uint32_t bass_at_last_status = 0;

  while (true) {
    // Status LED: toggles on every PC packet; falls back to a 1 s heartbeat when idle.
    if (receiver.poll(kPollTimeoutUs) > 0) {
      status_led = !status_led;
      gpio_put(kStatusLedPin, status_led);
      next_heartbeat = make_timeout_time_ms(kHeartbeatMs);
    } else if (time_reached(next_heartbeat)) {
      status_led = !status_led;
      gpio_put(kStatusLedPin, status_led);
      next_heartbeat = delayed_by_ms(next_heartbeat, kHeartbeatMs);
    }

    if (time_reached(next_status)) {
      next_status = delayed_by_ms(next_status, kStatusIntervalMs);
      const uint32_t renders = g_renderer.renders;
      const uint32_t frames = receiver.frames_ok();
      const uint32_t bass = receiver.bass_ok();
      printf("TVL fw=%lu board=%lu fps=%lu rx=%lu bass=%lu bad=%lu leds=%u ma=%lu limited=%lu\n",
             static_cast<unsigned long>(kFirmwareVersion), static_cast<unsigned long>(board_id),
             static_cast<unsigned long>(renders - renders_at_last_status),
             static_cast<unsigned long>(frames - frames_at_last_status),
             static_cast<unsigned long>(bass - bass_at_last_status),
             static_cast<unsigned long>(receiver.packets_bad()), receiver.led_count(),
             static_cast<unsigned long>(g_renderer.last_milliamps),
             static_cast<unsigned long>(g_renderer.limited_frames));
      renders_at_last_status = renders;
      frames_at_last_status = frames;
      bass_at_last_status = bass;
    }
  }
}
