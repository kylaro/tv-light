#include "frame_receiver.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "pico/stdio.h"
#include "pico/time.h"

namespace tvlight::fw {
namespace {
constexpr float kTenths = 10.0f;
constexpr float kMaxEncoded = 255.0f;
constexpr float kMaxLinear = 65535.0f;
constexpr float kHalf = 0.5f;
constexpr size_t kKindOffset = offsetof(protocol::PacketPrefix, kind);
constexpr unsigned kByteBits = 8;
}  // namespace

uint32_t FrameReceiver::poll(uint32_t timeout_us) {
  char buf[kReadChunk];
  const int n = stdio_get_until(buf, sizeof(buf), make_timeout_time_us(timeout_us));
  uint32_t packets = 0;
  for (int i = 0; i < n; ++i) packets += feed(static_cast<uint8_t>(buf[i]));
  return packets;
}

bool FrameReceiver::feed(uint8_t byte) {
  using protocol::PacketPrefix;
  // Hunt for the magic one byte at a time so we resync after garbage.
  if (pos_ < sizeof(protocol::kMagic)) {
    if (byte == protocol::kMagic[pos_]) {
      packet_[pos_++] = byte;
    } else {
      pos_ = (byte == protocol::kMagic[0]) ? 1 : 0;
      packet_[0] = protocol::kMagic[0];
    }
    return false;
  }
  packet_[pos_++] = byte;

  if (pos_ == sizeof(PacketPrefix)) {
    PacketPrefix prefix;
    std::memcpy(&prefix, packet_.data(), sizeof(prefix));
    expected_ = 0;
    if (prefix.version == protocol::kVersion) {
      if (prefix.kind == protocol::kKindBass) expected_ = protocol::kBassPacketSize;
      if (prefix.kind == protocol::kKindFrame) expected_ = sizeof(protocol::FrameHeader);  // refined below
    }
    if (expected_ == 0) {
      ++packets_bad_;
      pos_ = 0;
      return false;
    }
  }
  if (pos_ == sizeof(protocol::FrameHeader) && packet_[kKindOffset] == protocol::kKindFrame) {
    protocol::FrameHeader header;
    std::memcpy(&header, packet_.data(), sizeof(header));
    if (header.led_count > protocol::kMaxLeds) {
      ++packets_bad_;
      pos_ = 0;
      return false;
    }
    expected_ = protocol::frame_packet_size(header.led_count);
  }
  if (pos_ > sizeof(PacketPrefix) && pos_ == expected_) {
    pos_ = 0;
    return accept_packet();
  }
  return false;
}

bool FrameReceiver::accept_packet() {
  const size_t body = expected_ - protocol::kChecksumBytes;
  const uint16_t want = static_cast<uint16_t>(packet_[body] | (packet_[body + 1] << kByteBits));
  if (protocol::fletcher16(packet_.data(), body) != want) {
    ++packets_bad_;
    return false;
  }
  if (packet_[kKindOffset] == protocol::kKindBass) {
    accept_bass();
  } else {
    accept_frame();
  }
  return true;
}

void FrameReceiver::accept_bass() {
  protocol::BassPacket p;
  std::memcpy(&p, packet_.data(), sizeof(p));
  store_.post_bass(p.bass);
  ++bass_ok_;
}

void FrameReceiver::accept_frame() {
  using protocol::FrameHeader;
  FrameHeader header;
  std::memcpy(&header, packet_.data(), sizeof(header));
  if (header.gamma_tenths != gamma_tenths_) rebuild_gamma(header.gamma_tenths);

  const uint8_t* colors = packet_.data() + sizeof(FrameHeader);
  const size_t channels = header.led_count * protocol::kBytesPerLed;

  LinearFrame& f = store_.begin_write();
  f.led_count = header.led_count;
  f.brightness = header.brightness;
  f.always_on_share = header.always_on_share;
  f.flags = header.flags;
  f.max_current_ma = header.max_current_ma;
  f.dark_leds = std::min(header.dark_leds, header.led_count);
  for (size_t c = 0; c < protocol::kBytesPerLed; ++c) f.floor_lin[c] = gamma_lut_[header.floor_color[c]];
  for (size_t i = 0; i < channels; ++i) f.lin[i] = gamma_lut_[colors[i]];
  store_.publish();

  ++frames_ok_;
  led_count_ = header.led_count;
}

void FrameReceiver::rebuild_gamma(uint8_t gamma_tenths) {
  gamma_tenths_ = gamma_tenths;
  const float gamma = gamma_tenths > 0 ? gamma_tenths / kTenths : 1.0f;
  for (size_t v = 0; v < gamma_lut_.size(); ++v) {
    gamma_lut_[v] = static_cast<uint16_t>(std::pow(v / kMaxEncoded, gamma) * kMaxLinear + kHalf);
  }
}

}  // namespace tvlight::fw
