// tvlightd: ambient TV backlight daemon.
//   screen (gamescope / portal) + sink monitor bass -> Pico over USB serial,
//   with a web UI on the LAN.
#include <sys/resource.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "audio_capture.h"
#include "compositor.h"
#include "config.h"
#include "pw_loop.h"
#include "serial_link.h"
#include "video_capture.h"
#include "web_server.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kSourceTickInterval = std::chrono::milliseconds(250);
// Run below normal priority so a game's threads always win the CPU.
constexpr int kNiceLevel = 10;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::string default_config_path() {
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/tvlight/config.json";
  if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.config/tvlight/config.json";
  return "tvlight.json";
}

void usage(const char* argv0) {
  std::cerr << "usage: " << argv0 << " [--config PATH] [--web-dir DIR] [--port N]\n";
}
}  // namespace

int main(int argc, char** argv) {
  std::string config_path = default_config_path();
  std::string web_dir = TVLIGHT_SOURCE_WEB_DIR;
  int port_override = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--config" && has_value) {
      config_path = argv[++i];
    } else if (arg == "--web-dir" && has_value) {
      web_dir = argv[++i];
    } else if (arg == "--port" && has_value) {
      port_override = std::atoi(argv[++i]);
    } else {
      usage(argv[0]);
      return arg == "--help" || arg == "-h" ? 0 : 2;
    }
  }

  if (setpriority(PRIO_PROCESS, 0, kNiceLevel) != 0) std::cerr << "tvlightd: could not lower priority\n";
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  try {
    tvlight::ConfigStore config(config_path);
    config.load();
    tvlight::PwLoop pw;
    tvlight::VideoCapture video(pw, config);
    tvlight::AudioCapture audio(pw, config);
    tvlight::SerialLink serial(config);
    tvlight::Compositor compositor(config, video, audio, serial);
    tvlight::WebServer web(config, video, audio, serial, compositor, web_dir);

    audio.on_bass([&serial](float bass) { serial.post_bass(bass); });
    video.start();
    audio.start();
    serial.start();
    if (!web.start(port_override ? port_override : config.get().web_port)) return 1;

    auto last = Clock::now();
    auto next_frame = last;
    auto next_source_tick = last;
    while (!g_stop) {
      if (pw.disconnected()) {
        std::cerr << "tvlightd: exiting so the service manager restarts us\n";
        return 1;
      }
      const auto now = Clock::now();
      if (now >= next_source_tick) {
        video.tick();
        audio.tick();
        next_source_tick = now + kSourceTickInterval;
      }
      compositor.step(std::chrono::duration<double>(now - last).count());
      last = now;
      const auto frame = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / config.get().fps));
      next_frame += frame;
      if (next_frame < now) next_frame = now + frame;  // fell behind: don't burst
      std::this_thread::sleep_until(next_frame);
    }
    std::cerr << "tvlightd: shutting down\n";
  } catch (const std::exception& e) {
    std::cerr << "tvlightd: fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
