// Screen capture over PipeWire. Two sources:
//  - gamescope (SteamOS Game Mode) publishes a "gamescope" Video/Source node we
//    can link to directly, no permission prompt.
//  - xdg-desktop-portal ScreenCast (Desktop Mode, and GNOME on the dev box).
// Frames are reduced to a small grid of linear-light colors.
#pragma once

#include <spa/utils/hook.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "portal.h"
#include "pw_loop.h"

struct pw_stream;
struct pw_registry;
struct spa_pod;
struct spa_pod_builder;

namespace tvlight {

constexpr int kGridW = 64;
constexpr int kGridH = 36;
constexpr int kRgb = 3;

struct VideoGrid {
  uint64_t seq = 0;
  std::vector<float> lin = std::vector<float>(kGridW * kGridH * kRgb, 0.0f);  // row major, linear RGB
};

struct VideoStatus {
  std::string source = "none";   // none | gamescope | portal
  std::string state = "idle";
  std::string message;
  int width = 0;
  int height = 0;
  std::string format;
  double fps = 0;
  bool gamescope_available = false;
  uint64_t buffers = 0;    // buffers received from the producer
  uint64_t empty = 0;      // buffers without usable pixels (cursor-only updates, corrupted, ...)
  uint64_t throttled = 0;  // skipped by the capture fps cap
  uint64_t processed = 0;
};

class VideoCapture {
 public:
  VideoCapture(PwLoop& pw, ConfigStore& config);
  ~VideoCapture();
  void start();
  // Source selection / reconnect state machine; call a few times per second.
  void tick();
  // Drop the current portal session (or a denied request) and ask again.
  void request_portal();
  // Copies the newest grid if it is newer than `grid.seq`.
  bool latest(VideoGrid& grid);
  VideoStatus status();

 private:
  enum class Source { None, Gamescope, Portal };
  enum class PortalState { Idle, Requesting, Ready, Streaming, Failed };
  using Clock = std::chrono::steady_clock;

  void start_stream(Source source, pw_core* core, uint32_t target_id, const std::string& target_object);
  // EnumFormat param. The max frame rate hint makes the portal compositor copy fewer frames.
  static const spa_pod* build_format(spa_pod_builder* b, bool max_rate_hint, int fps);
  void teardown_locked();
  void set_status(const std::string& state, const std::string& message);
  void rebuild_lut(double gamma);

  // PipeWire callbacks (loop thread).
  static void on_registry_global(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version,
                                 const struct spa_dict* props);
  static void on_registry_global_remove(void* data, uint32_t id);
  static void on_stream_state(void* data, int old_state, int state, const char* error);
  static void on_stream_param_changed(void* data, uint32_t id, const spa_pod* param);
  static void on_stream_process(void* data);
  void process_frame(const uint8_t* pixels, int stride);

  PwLoop& pw_;
  ConfigStore& config_;
  Portal portal_;

  pw_registry* registry_ = nullptr;
  spa_hook registry_listener_{};
  uint32_t gamescope_id_ = UINT32_MAX;   // loop thread
  std::string gamescope_serial_;
  std::atomic<bool> gamescope_present_{false};

  Source source_ = Source::None;
  pw_stream* stream_ = nullptr;
  spa_hook stream_listener_{};
  pw_core* portal_core_ = nullptr;
  std::atomic<bool> stream_dead_{false};
  std::atomic<bool> portal_reset_{false};

  std::mutex portal_mutex_;
  PortalState portal_state_ = PortalState::Idle;
  PortalResult portal_result_;
  Clock::time_point portal_retry_at_{};

  // Negotiated format (loop thread).
  uint32_t format_ = 0;
  int width_ = 0;
  int height_ = 0;
  std::array<int, kRgb> channel_offset_{};
  std::array<float, 256> lut_{};
  double lut_gamma_ = 0;
  std::atomic<double> gamma_{2.2};
  std::atomic<int> capture_fps_{30};
  int requested_fps_ = 0;           // capture rate last put in the portal stream's format
  int64_t last_processed_ns_ = 0;   // throttle for producers that ignore the hint

  std::mutex mutex_;
  VideoGrid grid_;
  VideoStatus status_;
  uint64_t frames_ = 0;
  uint64_t frames_at_fps_ = 0;
  Clock::time_point fps_at_ = Clock::now();
};

}  // namespace tvlight
