// LAN web UI + JSON API (phone friendly). Serves static files from the web dir.
#pragma once

#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace tvlight {

class ConfigStore;
class VideoCapture;
class AudioCapture;
class SerialLink;
class Compositor;

class WebServer {
 public:
  WebServer(ConfigStore& config, VideoCapture& video, AudioCapture& audio, SerialLink& serial, Compositor& compositor,
            std::string web_dir);
  ~WebServer();
  bool start(int port);

 private:
  ConfigStore& config_;
  VideoCapture& video_;
  AudioCapture& audio_;
  SerialLink& serial_;
  Compositor& compositor_;
  std::string web_dir_;
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
};

}  // namespace tvlight
