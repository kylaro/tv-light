// USB CDC link to the Pico: finds the port, writes frames, reads status lines.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"

namespace tvlight {

struct SerialStatus {
  bool connected = false;
  std::string port;
  std::string last_line;                    // newest status line from the Pico
  std::map<std::string, std::string> fields;  // parsed key=value pairs of that line
  uint64_t sent = 0;       // color frames written
  uint64_t bass_sent = 0;  // bass packets written
  uint64_t dropped = 0;    // packets replaced before they could be written, or not writable
  std::string error;
};

class SerialLink {
 public:
  explicit SerialLink(ConfigStore& config);
  ~SerialLink();
  void start();
  // Queue the newest packet of each kind. Never blocks: a dedicated writer
  // thread sends them, and an unsent packet is simply replaced by a newer one.
  void post_frame(std::vector<uint8_t> packet);
  void post_bass(float bass);
  SerialStatus status() const;

 private:
  void run();
  void write_loop();
  bool write_all(int fd, const uint8_t* data, size_t size);
  bool try_open(const std::string& wanted);
  void close_locked(const std::string& why);
  void handle_line(const std::string& line);

  ConfigStore& config_;
  std::thread thread_;
  std::thread writer_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::vector<uint8_t> frame_;
  bool frame_pending_ = false;
  uint16_t bass_ = 0;
  bool bass_pending_ = false;
  std::atomic<bool> stop_{false};
  mutable std::mutex mutex_;
  int fd_ = -1;
  SerialStatus status_;
  std::string rx_;
};

}  // namespace tvlight
