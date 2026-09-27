// Owns the PipeWire thread loop, context and the default core connection.
#pragma once

#include <pipewire/pipewire.h>

#include <atomic>
#include <cerrno>
#include <iostream>
#include <stdexcept>

namespace tvlight {

class PwLoop {
 public:
  PwLoop() {
    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("tvlight-pw", nullptr);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!loop_ || !context_) throw std::runtime_error("pipewire: cannot create loop/context");
    if (pw_thread_loop_start(loop_) < 0) throw std::runtime_error("pipewire: cannot start loop");
    Lock lock(*this);
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) throw std::runtime_error("pipewire: cannot connect (is PIPEWIRE_RUNTIME_DIR/XDG_RUNTIME_DIR set?)");
    static pw_core_events events = [] {
      pw_core_events e{};
      e.version = PW_VERSION_CORE_EVENTS;
      e.error = &PwLoop::on_core_error;
      return e;
    }();
    pw_core_add_listener(core_, &core_listener_, &events, this);
  }
  ~PwLoop() {
    pw_thread_loop_stop(loop_);
    if (core_) pw_core_disconnect(core_);
    pw_context_destroy(context_);
    pw_thread_loop_destroy(loop_);
    pw_deinit();
  }
  PwLoop(const PwLoop&) = delete;
  PwLoop& operator=(const PwLoop&) = delete;

  // True once the PipeWire daemon went away (e.g. restarted). Streams on this
  // connection are dead; the process exits and systemd restarts it cleanly.
  bool disconnected() const { return disconnected_.load(); }

  pw_thread_loop* loop() const { return loop_; }
  pw_context* context() const { return context_; }
  pw_core* core() const { return core_; }

  class Lock {
   public:
    explicit Lock(PwLoop& l) : loop_(l.loop_) { pw_thread_loop_lock(loop_); }
    ~Lock() { pw_thread_loop_unlock(loop_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    pw_thread_loop* loop_;
  };

 private:
  pw_thread_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  spa_hook core_listener_{};
  std::atomic<bool> disconnected_{false};

  static void on_core_error(void* data, uint32_t id, int, int res, const char* message) {
    if (id != PW_ID_CORE || res != -EPIPE) return;
    std::cerr << "pipewire: connection lost (" << (message ? message : "?") << ")\n";
    static_cast<PwLoop*>(data)->disconnected_ = true;
  }
};

}  // namespace tvlight
