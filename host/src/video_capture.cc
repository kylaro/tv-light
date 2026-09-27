#include "video_capture.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

namespace tvlight {
namespace {
constexpr const char* kGamescopeNodeName = "gamescope";
constexpr int kBytesPerPixel = 4;
constexpr int kSamplesPerCellAxis = 6;  // sample at most 6x6 pixels per grid cell
constexpr size_t kPodBufferSize = 1024;
constexpr float kMaxByte = 255.0f;

// Ask producers that can scale (gamescope) for a small frame; others keep native size.
constexpr spa_rectangle kRequestSize{320, 180};
constexpr spa_rectangle kMinSize{1, 1};
constexpr spa_rectangle kMaxSize{16384, 16384};
constexpr spa_fraction kDefaultRate{60, 1};
// Process a frame once this fraction of the capture interval has passed, so a
// 60 Hz producer capped to 30 fps keeps every second frame instead of drifting.
constexpr double kThrottleSlack = 0.8;
constexpr double kNsPerSecond = 1e9;
constexpr spa_fraction kMinRate{0, 1};
constexpr spa_fraction kMaxRate{1000, 1};
constexpr int kMinBuffers = 2;
constexpr int kDefaultBuffers = 4;
constexpr int kMaxBuffers = 8;

constexpr auto kPortalRetryAfterError = std::chrono::seconds(30);
constexpr auto kFpsWindow = std::chrono::seconds(1);

// Byte offsets of R, G, B inside a 4-byte pixel, or false if unsupported.
bool channel_offsets(uint32_t format, std::array<int, kRgb>& out) {
  switch (format) {
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA: out = {2, 1, 0}; return true;
    case SPA_VIDEO_FORMAT_RGBx:
    case SPA_VIDEO_FORMAT_RGBA: out = {0, 1, 2}; return true;
    case SPA_VIDEO_FORMAT_xRGB:
    case SPA_VIDEO_FORMAT_ARGB: out = {1, 2, 3}; return true;
    case SPA_VIDEO_FORMAT_xBGR:
    case SPA_VIDEO_FORMAT_ABGR: out = {3, 2, 1}; return true;
    default: return false;
  }
}

const char* format_name(uint32_t format) {
  switch (format) {
    case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
    case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
    case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
    case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
    case SPA_VIDEO_FORMAT_xRGB: return "xRGB";
    case SPA_VIDEO_FORMAT_ARGB: return "ARGB";
    case SPA_VIDEO_FORMAT_xBGR: return "xBGR";
    case SPA_VIDEO_FORMAT_ABGR: return "ABGR";
    default: return "unsupported";
  }
}

const pw_registry_events kRegistryEvents = [] {
  pw_registry_events e{};
  e.version = PW_VERSION_REGISTRY_EVENTS;
  return e;
}();
}  // namespace

VideoCapture::VideoCapture(PwLoop& pw, ConfigStore& config) : pw_(pw), config_(config) {
  portal_.on_closed([this] { stream_dead_ = true; });
}

VideoCapture::~VideoCapture() {
  PwLoop::Lock lock(pw_);
  teardown_locked();
  if (registry_) {
    spa_hook_remove(&registry_listener_);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
  }
}

void VideoCapture::start() {
  static pw_registry_events events = kRegistryEvents;
  events.global = &VideoCapture::on_registry_global;
  events.global_remove = &VideoCapture::on_registry_global_remove;
  PwLoop::Lock lock(pw_);
  registry_ = pw_core_get_registry(pw_.core(), PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(registry_, &registry_listener_, &events, this);
}

void VideoCapture::on_registry_global(void* data, uint32_t id, uint32_t, const char* type, uint32_t,
                                      const spa_dict* props) {
  auto* self = static_cast<VideoCapture*>(data);
  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
  const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!name || std::strcmp(name, kGamescopeNodeName) != 0) return;
  if (media_class && std::strcmp(media_class, "Video/Source") != 0) return;
  const char* serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
  self->gamescope_id_ = id;
  self->gamescope_serial_ = serial ? serial : std::to_string(id);
  self->gamescope_present_ = true;
  std::cerr << "video: gamescope node appeared (id " << id << ")\n";
}

void VideoCapture::on_registry_global_remove(void* data, uint32_t id) {
  auto* self = static_cast<VideoCapture*>(data);
  if (id != self->gamescope_id_) return;
  self->gamescope_id_ = UINT32_MAX;
  self->gamescope_present_ = false;
  if (self->source_ == Source::Gamescope) self->stream_dead_ = true;
  std::cerr << "video: gamescope node went away\n";
}

void VideoCapture::request_portal() { portal_reset_ = true; }

void VideoCapture::tick() {
  const Config cfg = config_.get();
  gamma_ = cfg.screen_gamma;
  capture_fps_ = cfg.capture_fps;

  const bool allow_gamescope = cfg.capture_source == "auto" || cfg.capture_source == "gamescope";
  const bool allow_portal = cfg.capture_source == "auto" || cfg.capture_source == "portal";
  Source desired = Source::None;
  if (allow_gamescope && gamescope_present_) {
    desired = Source::Gamescope;
  } else if (allow_portal) {
    desired = Source::Portal;
  }

  PwLoop::Lock lock(pw_);
  if (portal_reset_.exchange(false)) {
    std::lock_guard plock(portal_mutex_);
    if (portal_state_ != PortalState::Requesting) {
      if (source_ == Source::Portal) teardown_locked();
      if (portal_state_ == PortalState::Ready && portal_result_.pipewire_fd >= 0) close(portal_result_.pipewire_fd);
      portal_state_ = PortalState::Idle;
    }
  }
  if (stream_ && (desired != source_ || stream_dead_)) {
    std::cerr << "video: stopping stream (" << (stream_dead_ ? "ended" : "source change") << ")\n";
    teardown_locked();
  }
  if (desired != Source::Portal) {
    std::lock_guard plock(portal_mutex_);
    if (portal_state_ == PortalState::Streaming || portal_state_ == PortalState::Ready) {
      if (portal_state_ == PortalState::Ready && portal_result_.pipewire_fd >= 0) close(portal_result_.pipewire_fd);
      portal_.close();
      portal_state_ = PortalState::Idle;
    }
  }

  switch (desired) {
    case Source::None:
      set_status("idle", cfg.capture_source == "none" ? "capture disabled" : "no capture source available");
      break;
    case Source::Gamescope:
      if (!stream_) start_stream(Source::Gamescope, pw_.core(), PW_ID_ANY, gamescope_serial_);
      break;
    case Source::Portal: {
      std::lock_guard plock(portal_mutex_);
      switch (portal_state_) {
        case PortalState::Streaming:
          if (!stream_) portal_state_ = PortalState::Idle;  // stream ended: reopen with the restore token
          break;
        case PortalState::Failed:
          if (Clock::now() < portal_retry_at_) break;
          portal_state_ = PortalState::Idle;
          [[fallthrough]];
        case PortalState::Idle:
          portal_state_ = PortalState::Requesting;
          set_status("waiting", "asking xdg-desktop-portal for screen access (check the TV for a dialog)");
          portal_.request(cfg.portal_restore_token, [this](PortalResult r) {
            std::lock_guard cb_lock(portal_mutex_);
            if (!r.restore_token.empty()) config_.set_restore_token(r.restore_token);
            if (r.ok) {
              std::cerr << "video: portal granted node " << r.node_id << " (" << r.width << "x" << r.height << ")\n";
              portal_result_ = std::move(r);
              portal_state_ = PortalState::Ready;
              return;
            }
            std::cerr << "video: portal: " << r.error << "\n";
            portal_state_ = PortalState::Failed;
            // A dismissed dialog waits for the user to press "request screen access".
            portal_retry_at_ = r.cancelled ? Clock::time_point::max() : Clock::now() + kPortalRetryAfterError;
            set_status(r.cancelled ? "denied" : "error", r.error);
          });
          break;
        case PortalState::Requesting:
          break;
        case PortalState::Ready: {
          portal_core_ = pw_context_connect_fd(pw_.context(), portal_result_.pipewire_fd, nullptr, 0);
          portal_result_.pipewire_fd = -1;  // owned by the core now
          if (!portal_core_) {
            portal_state_ = PortalState::Failed;
            portal_retry_at_ = Clock::now() + kPortalRetryAfterError;
            set_status("error", "cannot connect to portal PipeWire remote");
            break;
          }
          start_stream(Source::Portal, portal_core_, portal_result_.node_id, "");
          portal_state_ = PortalState::Streaming;
          break;
        }
      }
      break;
    }
  }

  if (stream_ && source_ == Source::Portal && requested_fps_ != cfg.capture_fps) {
    uint8_t buffer[kPodBufferSize];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    requested_fps_ = cfg.capture_fps;
    const spa_pod* params[1] = {build_format(&b, true, requested_fps_)};
    pw_stream_update_params(stream_, params, 1);
    std::cerr << "video: capture rate hint now " << requested_fps_ << " fps\n";
  }

  std::lock_guard slock(mutex_);
  status_.gamescope_available = gamescope_present_;
  const auto now = Clock::now();
  if (now - fps_at_ >= kFpsWindow) {
    status_.fps = static_cast<double>(frames_ - frames_at_fps_) / std::chrono::duration<double>(now - fps_at_).count();
    frames_at_fps_ = frames_;
    fps_at_ = now;
  }
}

void VideoCapture::start_stream(Source source, pw_core* core, uint32_t target_id, const std::string& target_object) {
  static pw_stream_events events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = reinterpret_cast<void (*)(void*, pw_stream_state, pw_stream_state, const char*)>(
        &VideoCapture::on_stream_state);
    e.param_changed = &VideoCapture::on_stream_param_changed;
    e.process = &VideoCapture::on_stream_process;
    return e;
  }();

  pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                           PW_KEY_MEDIA_ROLE, "Screen", nullptr);
  if (!target_object.empty()) pw_properties_set(props, PW_KEY_TARGET_OBJECT, target_object.c_str());
  stream_ = pw_stream_new(core, "tvlight-video", props);
  if (!stream_) {
    set_status("error", "pw_stream_new failed");
    return;
  }
  stream_dead_ = false;
  source_ = source;
  pw_stream_add_listener(stream_, &stream_listener_, &events, this);

  uint8_t buffer[kPodBufferSize];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  // gamescope already renders a GPU-scaled 320x180 copy; only the portal gets the rate hint.
  const bool hint = source == Source::Portal;
  requested_fps_ = capture_fps_;
  const spa_pod* params[1] = {build_format(&b, hint, requested_fps_)};

  const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
  const int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, target_id, flags, params, 1);
  const char* name = source == Source::Gamescope ? "gamescope" : "portal";
  if (res < 0) {
    set_status("error", std::string("connect failed: ") + spa_strerror(res));
    stream_dead_ = true;
    return;
  }
  {
    std::lock_guard lock(mutex_);
    status_.source = name;
  }
  set_status("connecting", std::string("linking to ") + name + " stream");
  std::cerr << "video: connecting to " << name << " stream\n";
}

const spa_pod* VideoCapture::build_format(spa_pod_builder* b, bool max_rate_hint, int fps) {
  spa_pod_frame f;
  spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add(
      b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
      SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
      SPA_POD_CHOICE_ENUM_Id(9, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
                             SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_xRGB, SPA_VIDEO_FORMAT_xBGR,
                             SPA_VIDEO_FORMAT_ARGB, SPA_VIDEO_FORMAT_ABGR),
      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&kRequestSize, &kMinSize, &kMaxSize),
      SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&kDefaultRate, &kMinRate, &kMaxRate), 0);
  if (max_rate_hint) {
    const spa_fraction max_rate{static_cast<uint32_t>(fps), 1};
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate,
                        SPA_POD_CHOICE_RANGE_Fraction(&max_rate, &kMinRate, &max_rate), 0);
  }
  return static_cast<const spa_pod*>(spa_pod_builder_pop(b, &f));
}

void VideoCapture::teardown_locked() {
  if (stream_) {
    spa_hook_remove(&stream_listener_);
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }
  if (portal_core_) {
    pw_core_disconnect(portal_core_);
    portal_core_ = nullptr;
  }
  source_ = Source::None;
  stream_dead_ = false;
  std::lock_guard lock(mutex_);
  status_.source = "none";
  status_.width = status_.height = 0;
  status_.format.clear();
}

void VideoCapture::set_status(const std::string& state, const std::string& message) {
  std::lock_guard lock(mutex_);
  status_.state = state;
  status_.message = message;
}

void VideoCapture::on_stream_state(void* data, int, int state, const char* error) {
  auto* self = static_cast<VideoCapture*>(data);
  const auto s = static_cast<pw_stream_state>(state);
  std::cerr << "video: stream " << pw_stream_state_as_string(s) << (error ? std::string(": ") + error : "") << "\n";
  switch (s) {
    case PW_STREAM_STATE_ERROR:
      self->set_status("error", error ? error : "stream error");
      self->stream_dead_ = true;
      break;
    case PW_STREAM_STATE_UNCONNECTED:
      self->stream_dead_ = true;
      break;
    case PW_STREAM_STATE_PAUSED:
      self->set_status("paused", "stream negotiated, waiting for frames");
      break;
    case PW_STREAM_STATE_STREAMING:
      self->set_status("streaming", "");
      break;
    default:
      break;
  }
}

void VideoCapture::on_stream_param_changed(void* data, uint32_t id, const spa_pod* param) {
  auto* self = static_cast<VideoCapture*>(data);
  if (id != SPA_PARAM_Format || !param) return;
  spa_video_info_raw info{};
  if (spa_format_video_raw_parse(param, &info) < 0) return;
  self->format_ = info.format;
  self->width_ = static_cast<int>(info.size.width);
  self->height_ = static_cast<int>(info.size.height);
  if (!channel_offsets(info.format, self->channel_offset_)) {
    self->set_status("error", std::string("unsupported pixel format ") + format_name(info.format));
  }
  {
    std::lock_guard lock(self->mutex_);
    self->status_.width = self->width_;
    self->status_.height = self->height_;
    self->status_.format = format_name(info.format);
  }
  std::cerr << "video: format " << format_name(info.format) << " " << self->width_ << "x" << self->height_ << "\n";

  // Only shared memory buffers: we read pixels on the CPU and skip DMA-BUF import.
  uint8_t buffer[kPodBufferSize];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const spa_pod* params[1];
  params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
      &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
      SPA_POD_CHOICE_RANGE_Int(kDefaultBuffers, kMinBuffers, kMaxBuffers), SPA_PARAM_BUFFERS_dataType,
      SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
  pw_stream_update_params(self->stream_, params, 1);
}

void VideoCapture::on_stream_process(void* data) {
  auto* self = static_cast<VideoCapture*>(data);
  // Drain the queue and keep only the newest buffer.
  pw_buffer* newest = nullptr;
  while (pw_buffer* b = pw_stream_dequeue_buffer(self->stream_)) {
    if (newest) pw_stream_queue_buffer(self->stream_, newest);
    newest = b;
  }
  if (!newest) return;
  {
    std::lock_guard lock(self->mutex_);
    ++self->status_.buffers;
  }
  const int64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  const double min_interval_ns = kNsPerSecond / self->capture_fps_ * kThrottleSlack;
  if (now_ns - self->last_processed_ns_ < min_interval_ns) {
    std::lock_guard lock(self->mutex_);
    ++self->status_.throttled;
    pw_stream_queue_buffer(self->stream_, newest);
    return;
  }
  const spa_buffer* buf = newest->buffer;
  bool processed = false;
  if (buf->n_datas > 0) {
    const spa_data& d = buf->datas[0];
    const bool usable = d.data && d.chunk && d.chunk->size > 0 && !(d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED);
    if (usable && self->width_ > 0) {
      const int stride = d.chunk->stride > 0 ? d.chunk->stride : self->width_ * kBytesPerPixel;
      const size_t needed = static_cast<size_t>(stride) * self->height_;
      if (d.chunk->offset + needed <= d.maxsize) {
        self->process_frame(static_cast<const uint8_t*>(d.data) + d.chunk->offset, stride);
        self->last_processed_ns_ = now_ns;
        processed = true;
      }
    }
  }
  if (!processed) {
    std::lock_guard lock(self->mutex_);
    ++self->status_.empty;
  }
  pw_stream_queue_buffer(self->stream_, newest);
}

void VideoCapture::rebuild_lut(double gamma) {
  lut_gamma_ = gamma;
  for (size_t v = 0; v < lut_.size(); ++v) lut_[v] = static_cast<float>(std::pow(v / kMaxByte, gamma));
}

void VideoCapture::process_frame(const uint8_t* pixels, int stride) {
  std::array<int, kRgb> off{};
  if (!channel_offsets(format_, off)) return;
  const double gamma = gamma_;
  if (gamma != lut_gamma_) rebuild_lut(gamma);

  VideoGrid next;
  for (int gy = 0; gy < kGridH; ++gy) {
    const int y0 = gy * height_ / kGridH;
    const int y1 = std::max(y0 + 1, (gy + 1) * height_ / kGridH);
    const int step_y = std::max(1, (y1 - y0) / kSamplesPerCellAxis);
    for (int gx = 0; gx < kGridW; ++gx) {
      const int x0 = gx * width_ / kGridW;
      const int x1 = std::max(x0 + 1, (gx + 1) * width_ / kGridW);
      const int step_x = std::max(1, (x1 - x0) / kSamplesPerCellAxis);
      float sum[kRgb] = {0, 0, 0};
      int count = 0;
      for (int y = y0; y < y1 && y < height_; y += step_y) {
        const uint8_t* row = pixels + static_cast<size_t>(y) * stride;
        for (int x = x0; x < x1 && x < width_; x += step_x) {
          const uint8_t* px = row + x * kBytesPerPixel;
          for (int c = 0; c < kRgb; ++c) sum[c] += lut_[px[off[c]]];
          ++count;
        }
      }
      float* cell = &next.lin[(gy * kGridW + gx) * kRgb];
      for (int c = 0; c < kRgb; ++c) cell[c] = count ? sum[c] / count : 0.0f;
    }
  }

  std::lock_guard lock(mutex_);
  next.seq = grid_.seq + 1;
  ++status_.processed;
  grid_ = std::move(next);
  ++frames_;
}

bool VideoCapture::latest(VideoGrid& grid) {
  std::lock_guard lock(mutex_);
  if (grid_.seq == grid.seq) return false;
  grid = grid_;
  return true;
}

VideoStatus VideoCapture::status() {
  std::lock_guard lock(mutex_);
  return status_;
}

}  // namespace tvlight
