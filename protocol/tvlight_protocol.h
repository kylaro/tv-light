// Wire protocol between tvlightd (host) and the Pico firmware.
// Shared by both sides; keep it dependency free (C++17, <cstdint> only).
//
// Host -> Pico, over USB CDC serial. Every packet starts with PacketPrefix
// and ends with a uint16 Fletcher-16 checksum (LE) over everything before it.
//
//   kKindFrame: FrameHeader | led_count * 3 color bytes | checksum     (screen rate, ~60 Hz)
//   kKindBass:  BassPacket                              | checksum     (audio block rate, ~190 Hz)
//
// Bass travels separately so the pump follows the audio at its own rate
// instead of waiting for the next screen frame; the Pico renders both at
// 144+ Hz.
//
// Colors are gamma encoded 8-bit values in *wire order* (the host already
// reorders RGB into the strip's byte order, e.g. GRB for WS2812B). The Pico
// converts them to linear light with `gamma_tenths`, applies the bass share of brightness,
// brightness, current limiting and temporal dithering, then drives the strip.
//
// Pico -> host: newline terminated ASCII status lines, e.g.
//   "TVL fw=3 board=0 fps=245 rx=60 bass=188 bad=0 leds=124 ma=2310 limited=0"
#pragma once

#include <cstddef>
#include <cstdint>

namespace tvlight::protocol {

constexpr uint8_t kMagic[3] = {'T', 'V', 'L'};
constexpr uint8_t kVersion = 3;
constexpr uint16_t kMaxLeds = 600;
constexpr size_t kBytesPerLed = 3;
constexpr size_t kChecksumBytes = 2;

enum Kind : uint8_t {
  kKindFrame = 'F',
  kKindBass = 'B',
};

// Brightness split: out = brightness * (base * (always_on + (1 - always_on) * bass) + floor * bass)
// with always_on = always_on_share / kShareOne.
constexpr uint8_t kShareOne = 255;
constexpr uint16_t kBassFullScale = 0xFFFF;
constexpr uint8_t kGammaTenthsDefault = 22;

enum Flags : uint8_t {
  kFlagDither = 1u << 0,       // temporal dithering 16-bit -> 8-bit
  kFlagInterpolate = 1u << 1,  // blend between frames at the render rate
};

#pragma pack(push, 1)
struct PacketPrefix {
  uint8_t magic[3];
  uint8_t version;
  uint8_t kind;  // Kind
};

struct FrameHeader {
  PacketPrefix prefix;
  uint16_t led_count;       // number of LEDs following the header
  uint8_t brightness;       // global brightness, 0..255
  uint8_t always_on_share;  // part of brightness that ignores bass, see kShareOne
  uint8_t floor_color[3];   // wire order, gamma encoded; added as floor * bass
  uint8_t flags;            // Flags
  uint8_t gamma_tenths;     // gamma exponent * 10 (22 -> 2.2)
  uint16_t max_current_ma;  // strip current budget, 0 = unlimited
};

struct BassPacket {
  PacketPrefix prefix;
  uint16_t bass;  // bass envelope, 0..kBassFullScale
};
#pragma pack(pop)

static_assert(sizeof(PacketPrefix) == 5, "PacketPrefix must be packed");
static_assert(sizeof(FrameHeader) == 16, "FrameHeader must be packed");
static_assert(sizeof(BassPacket) == 7, "BassPacket must be packed");

constexpr size_t frame_packet_size(uint16_t led_count) {
  return sizeof(FrameHeader) + led_count * kBytesPerLed + kChecksumBytes;
}
constexpr size_t kBassPacketSize = sizeof(BassPacket) + kChecksumBytes;

// Fletcher-16 over `len` bytes. Cheap enough for the RP2040 at 60+ fps.
inline uint16_t fletcher16(const uint8_t* data, size_t len) {
  constexpr uint32_t kModulus = 255;
  constexpr size_t kBlock = 360;  // keeps the uint32 sums from overflowing between reductions
  uint32_t sum1 = 0;
  uint32_t sum2 = 0;
  while (len > 0) {
    size_t block = len < kBlock ? len : kBlock;
    len -= block;
    while (block-- > 0) {
      sum1 += *data++;
      sum2 += sum1;
    }
    sum1 %= kModulus;
    sum2 %= kModulus;
  }
  return static_cast<uint16_t>((sum2 << 8) | sum1);
}

// Appends the checksum to a packet whose last kChecksumBytes are reserved for it.
inline void seal(uint8_t* packet, size_t size) {
  constexpr unsigned kByteBits = 8;
  constexpr uint16_t kByteMask = 0xFF;
  const size_t body = size - kChecksumBytes;
  const uint16_t sum = fletcher16(packet, body);
  packet[body] = static_cast<uint8_t>(sum & kByteMask);
  packet[body + 1] = static_cast<uint8_t>(sum >> kByteBits);
}

}  // namespace tvlight::protocol
