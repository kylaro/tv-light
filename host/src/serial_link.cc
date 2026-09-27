#include "serial_link.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>

#include "tvlight_protocol.h"

namespace tvlight {
namespace {
constexpr const char* kByIdDir = "/dev/serial/by-id";
// Our firmware sets the USB product to "TV Light"; fall back to any Pico.
constexpr const char* kPreferredIdPart = "TV_Light";
constexpr const char* kFallbackIdPart = "Raspberry_Pi";
constexpr auto kReopenDelay = std::chrono::seconds(1);
constexpr int kReadPollMs = 100;
constexpr int kWriteTimeoutMs = 20;
constexpr size_t kReadChunk = 512;
constexpr size_t kMaxLineLength = 1024;
constexpr auto kWriterWake = std::chrono::milliseconds(100);  // re-check stop_ periodically

std::string find_port() {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::string fallback;
  for (const auto& entry : fs::directory_iterator(kByIdDir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.find(kPreferredIdPart) != std::string::npos) return entry.path().string();
    if (fallback.empty() && name.find(kFallbackIdPart) != std::string::npos) fallback = entry.path().string();
  }
  return fallback;
}
}  // namespace

SerialLink::SerialLink(ConfigStore& config) : config_(config) {}

SerialLink::~SerialLink() {
  stop_ = true;
  queue_cv_.notify_all();
  if (writer_.joinable()) writer_.join();
  if (thread_.joinable()) thread_.join();
  std::lock_guard lock(mutex_);
  if (fd_ >= 0) ::close(fd_);
}

void SerialLink::start() {
  thread_ = std::thread([this] { run(); });
  writer_ = std::thread([this] { write_loop(); });
}

bool SerialLink::try_open(const std::string& wanted) {
  const std::string port = wanted == "auto" ? find_port() : wanted;
  if (port.empty()) {
    std::lock_guard lock(mutex_);
    status_.error = "no Pico found in /dev/serial/by-id";
    return false;
  }
  const int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    std::lock_guard lock(mutex_);
    status_.error = port + ": " + std::strerror(errno);
    return false;
  }
  termios tio{};
  tcgetattr(fd, &tio);
  cfmakeraw(&tio);
  tio.c_cflag |= CLOCAL | CREAD;
  cfsetspeed(&tio, B115200);  // ignored by USB CDC, but must not be 1200 (that reboots the Pico to BOOTSEL)
  tcsetattr(fd, TCSANOW, &tio);
  tcflush(fd, TCIOFLUSH);

  std::lock_guard lock(mutex_);
  fd_ = fd;
  status_.connected = true;
  status_.port = port;
  status_.error.clear();
  rx_.clear();
  std::cerr << "serial: opened " << port << "\n";
  return true;
}

void SerialLink::close_locked(const std::string& why) {
  if (fd_ >= 0) {
    ::close(fd_);
    std::cerr << "serial: closed " << status_.port << " (" << why << ")\n";
  }
  fd_ = -1;
  status_.connected = false;
  status_.error = why;
}

void SerialLink::run() {
  std::string opened_for;
  while (!stop_) {
    const std::string wanted = config_.get().serial_port;
    int fd;
    {
      std::lock_guard lock(mutex_);
      if (fd_ >= 0 && wanted != opened_for) close_locked("port setting changed");
      fd = fd_;
    }
    if (fd < 0) {
      if (!try_open(wanted)) {
        std::this_thread::sleep_for(kReopenDelay);
        continue;
      }
      opened_for = wanted;
      std::lock_guard lock(mutex_);
      fd = fd_;
    }

    pollfd pfd{fd, POLLIN, 0};
    const int r = ::poll(&pfd, 1, kReadPollMs);
    if (r <= 0) continue;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      std::lock_guard lock(mutex_);
      if (fd_ == fd) close_locked("device disconnected");
      continue;
    }
    char buf[kReadChunk];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      std::lock_guard lock(mutex_);
      if (fd_ == fd) close_locked(n == 0 ? "device disconnected" : std::strerror(errno));
      continue;
    }
    rx_.append(buf, static_cast<size_t>(n));
    size_t nl;
    while ((nl = rx_.find('\n')) != std::string::npos) {
      std::string line = rx_.substr(0, nl);
      rx_.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      handle_line(line);
    }
    if (rx_.size() > kMaxLineLength) rx_.clear();
  }
}

void SerialLink::handle_line(const std::string& line) {
  std::map<std::string, std::string> fields;
  std::istringstream in(line);
  std::string word;
  while (in >> word) {
    const size_t eq = word.find('=');
    if (eq != std::string::npos) fields[word.substr(0, eq)] = word.substr(eq + 1);
  }
  std::lock_guard lock(mutex_);
  status_.last_line = line;
  if (line.rfind("TVL ", 0) == 0) status_.fields = std::move(fields);
}

void SerialLink::post_frame(std::vector<uint8_t> packet) {
  {
    std::lock_guard lock(queue_mutex_);
    if (frame_pending_) {
      std::lock_guard slock(mutex_);
      ++status_.dropped;
    }
    frame_ = std::move(packet);
    frame_pending_ = true;
  }
  queue_cv_.notify_one();
}

void SerialLink::post_bass(float bass) {
  {
    std::lock_guard lock(queue_mutex_);
    bass_ = static_cast<uint16_t>(std::clamp(bass, 0.0f, 1.0f) * protocol::kBassFullScale);
    bass_pending_ = true;
  }
  queue_cv_.notify_one();
}

void SerialLink::write_loop() {
  std::vector<uint8_t> frame;
  std::array<uint8_t, protocol::kBassPacketSize> bass_packet{};
  while (!stop_) {
    bool send_frame = false;
    bool send_bass = false;
    {
      std::unique_lock lock(queue_mutex_);
      queue_cv_.wait_for(lock, kWriterWake, [this] { return stop_ || frame_pending_ || bass_pending_; });
      if (bass_pending_) {
        protocol::BassPacket p{{{protocol::kMagic[0], protocol::kMagic[1], protocol::kMagic[2]},
                                protocol::kVersion,
                                protocol::kKindBass},
                               bass_};
        std::memcpy(bass_packet.data(), &p, sizeof(p));
        protocol::seal(bass_packet.data(), bass_packet.size());
        bass_pending_ = false;
        send_bass = true;
      }
      if (frame_pending_) {
        frame.swap(frame_);
        frame_pending_ = false;
        send_frame = true;
      }
    }
    int fd;
    {
      std::lock_guard lock(mutex_);
      fd = fd_;
    }
    if (fd < 0) {
      std::lock_guard lock(mutex_);
      status_.dropped += send_bass + send_frame;
      continue;
    }
    // Bass first: it is tiny and the most latency sensitive.
    if (send_bass && write_all(fd, bass_packet.data(), bass_packet.size())) {
      std::lock_guard lock(mutex_);
      ++status_.bass_sent;
    }
    if (send_frame && write_all(fd, frame.data(), frame.size())) {
      std::lock_guard lock(mutex_);
      ++status_.sent;
    }
  }
}

bool SerialLink::write_all(int fd, const uint8_t* data, size_t size) {
  size_t done = 0;
  while (done < size) {
    const ssize_t n = ::write(fd, data + done, size - done);
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) {
      pollfd pfd{fd, POLLOUT, 0};
      if (::poll(&pfd, 1, kWriteTimeoutMs) > 0) continue;
      // Pico not draining; its checksum discards the partial packet.
    } else {
      std::lock_guard lock(mutex_);
      if (fd_ == fd) close_locked(std::strerror(errno));
    }
    std::lock_guard lock(mutex_);
    ++status_.dropped;
    return false;
  }
  return true;
}

SerialStatus SerialLink::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

}  // namespace tvlight
