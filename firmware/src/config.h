// Firmware tunables and FireFly V1.1 pin map. Everything the host can change
// lives in the packets instead (see protocol/tvlight_protocol.h).
#pragma once

#include <cstddef>
#include <cstdint>

#include "tvlight_protocol.h"

namespace tvlight::fw {

// FireFly V1.1 pins (see boards/firefly_v1_1.h).
constexpr unsigned kLedDataPin = 16;      // LED_OUT_3
constexpr unsigned kStatusLedPin = 10;    // STATUS_LED, active high
constexpr unsigned kBoardIdFirstPin = 0;  // BOARD_ID_0..4 on GPIO0..4, read-only straps
constexpr unsigned kBoardIdBits = 5;

constexpr uint32_t kWs2812BitHz = 800000;
constexpr uint32_t kUsPerLed = 30;               // 24 bits at 800 kHz
constexpr uint32_t kLatchUs = 300;               // WS2812B reset time is >= 280 us
constexpr uint32_t kMinRenderIntervalUs = 2000;  // cap at 500 Hz; strip length is the real limit

// With no frames for kFrameTimeoutMs, fade to black over kFadeOutMs.
constexpr uint32_t kFrameTimeoutMs = 1500;
constexpr uint32_t kFadeOutMs = 1000;

// Color interpolation length follows the measured host frame interval, clamped.
constexpr uint32_t kMinInterpUs = 4000;
constexpr uint32_t kMaxInterpUs = 100000;
constexpr uint32_t kDefaultInterpUs = 16667;

// Bass packets arrive every audio block (~5 ms); ramp across one interval.
constexpr uint32_t kMinBassInterpUs = 1000;
constexpr uint32_t kMaxBassInterpUs = 25000;
constexpr uint32_t kDefaultBassInterpUs = 5333;
constexpr uint32_t kBassTimeoutUs = 100000;  // no bass packets (audio idle) -> bass falls to 0

// Current model for the limiter: a WS2812B channel draws ~20 mA at full duty,
// plus ~1 mA quiescent per LED.
constexpr uint32_t kMilliampsPerChannel = 20;
constexpr uint32_t kIdleMicroampsPerLed = 1000;

// When the strip gets shorter, keep sending the old length this many times so
// the now-unused LEDs are blanked, then drop back to the short (faster) length.
constexpr uint32_t kBlankRefreshes = 3;

constexpr uint32_t kStatusIntervalMs = 1000;
// Status LED: heartbeat toggle every second while idle; toggles on every packet while the PC talks.
constexpr uint32_t kHeartbeatMs = 1000;
constexpr uint32_t kFirmwareVersion = 4;

constexpr size_t kChannels = protocol::kMaxLeds * protocol::kBytesPerLed;

}  // namespace tvlight::fw
