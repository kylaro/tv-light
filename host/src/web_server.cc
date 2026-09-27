#include "web_server.h"

#include <cstdio>
#include <iostream>

#include "audio_capture.h"
#include "compositor.h"
#include "config.h"
#include "httplib.h"
#include "serial_link.h"
#include "video_capture.h"

namespace tvlight {
namespace {
using nlohmann::json;
constexpr const char* kListenAddress = "0.0.0.0";
constexpr const char* kJson = "application/json";
constexpr int kBadRequest = 400;
constexpr size_t kHexBufferSize = 8;

std::string hex(uint32_t rgb) {
  char buf[kHexBufferSize];
  std::snprintf(buf, sizeof(buf), "%06x", rgb);
  return buf;
}
}  // namespace

WebServer::WebServer(ConfigStore& config, VideoCapture& video, AudioCapture& audio, SerialLink& serial,
                     Compositor& compositor, std::string web_dir)
    : config_(config),
      video_(video),
      audio_(audio),
      serial_(serial),
      compositor_(compositor),
      web_dir_(std::move(web_dir)),
      server_(std::make_unique<httplib::Server>()) {}

WebServer::~WebServer() {
  server_->stop();
  if (thread_.joinable()) thread_.join();
}

bool WebServer::start(int port) {
  auto& s = *server_;
  if (!s.set_mount_point("/", web_dir_)) std::cerr << "web: static dir " << web_dir_ << " not found\n";

  s.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
    const Snapshot snap = compositor_.snapshot();
    const VideoStatus v = video_.status();
    const AudioStatus a = audio_.status();
    const SerialStatus p = serial_.status();

    json leds = json::array();
    for (uint32_t c : snap.leds) leds.push_back(hex(c));
    json zones = json::array();
    for (const LedZone& z : snap.zones) zones.push_back({z.x, z.y, std::string(1, z.side)});

    json j = {
        {"fps", snap.fps},
        {"leds", leds},
        {"zones", zones},
        {"floor", hex(snap.floor)},
        {"bass", snap.bass},
        {"identify_s", snap.identify_s},
        {"led_count", snap.led_count},
        {"mapped", snap.mapped},
        {"config_version", config_.version()},
        {"video",
         {{"source", v.source},
          {"state", v.state},
          {"message", v.message},
          {"width", v.width},
          {"height", v.height},
          {"format", v.format},
          {"fps", v.fps},
          {"gamescope_available", v.gamescope_available},
          {"buffers", v.buffers},
          {"empty", v.empty},
          {"throttled", v.throttled},
          {"processed", v.processed}}},
        {"audio", {{"state", a.state}, {"bass", a.bass}, {"envelope", a.envelope}, {"reference", a.reference}}},
        {"pico",
         {{"connected", p.connected},
          {"port", p.port},
          {"line", p.last_line},
          {"fields", p.fields},
          {"sent", p.sent},
          {"bass_sent", p.bass_sent},
          {"dropped", p.dropped},
          {"error", p.error}}},
    };
    res.set_content(j.dump(), kJson);
  });

  s.Get("/api/thumb", [this](const httplib::Request&, httplib::Response& res) {
    const Snapshot snap = compositor_.snapshot();
    res.set_header("X-Width", std::to_string(kGridW));
    res.set_header("X-Height", std::to_string(kGridH));
    res.set_content(std::string(snap.thumbnail.begin(), snap.thumbnail.end()), "application/octet-stream");
  });

  s.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
    json j = config_.get();
    j.erase("portal_restore_token");
    res.set_content(j.dump(), kJson);
  });

  s.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      json patch = json::parse(req.body);
      patch.erase("portal_restore_token");
      json j = config_.patch(patch);
      j.erase("portal_restore_token");
      res.set_content(j.dump(), kJson);
    } catch (const std::exception& e) {
      res.status = kBadRequest;
      res.set_content(json{{"error", e.what()}}.dump(), kJson);
    }
  });

  s.Post("/api/capture/request", [this](const httplib::Request&, httplib::Response& res) {
    config_.set_restore_token("");  // force the picker so a different monitor can be chosen
    video_.request_portal();
    res.set_content(R"({"ok":true})", kJson);
  });

  if (!s.bind_to_port(kListenAddress, port)) {
    std::cerr << "web: cannot bind port " << port << "\n";
    return false;
  }
  thread_ = std::thread([this] { server_->listen_after_bind(); });
  std::cerr << "web: http://" << kListenAddress << ":" << port << "/\n";
  return true;
}

}  // namespace tvlight
