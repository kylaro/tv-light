// xdg-desktop-portal ScreenCast via libportal (Desktop Mode: KDE/GNOME).
// Runs its own GLib main loop thread; results are delivered on that thread.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <thread>

typedef struct _GObject GObject;
typedef struct _GAsyncResult GAsyncResult;
typedef struct _GMainContext GMainContext;
typedef struct _GMainLoop GMainLoop;
typedef struct _XdpPortal XdpPortal;
typedef struct _XdpSession XdpSession;

namespace tvlight {

struct PortalResult {
  bool ok = false;
  bool cancelled = false;  // the user dismissed the dialog
  int pipewire_fd = -1;    // caller owns it
  uint32_t node_id = 0;
  int width = 0;
  int height = 0;
  std::string restore_token;
  std::string error;
};

class Portal {
 public:
  using Callback = std::function<void(PortalResult)>;
  Portal();
  ~Portal();
  // Starts a screencast session (shows the picker unless the token is still valid).
  void request(std::string restore_token, Callback done);
  // Ends the current session, if any.
  void close();
  // Called (on the GLib thread) when the compositor ends the session.
  void on_closed(std::function<void()> cb) { closed_cb_ = std::move(cb); }

 private:
  void run_on_loop(std::function<void()> fn);
  void do_request(const std::string& token);
  void finish(PortalResult r);
  static void on_session_created(GObject* source, GAsyncResult* res, void* self);
  static void on_session_started(GObject* source, GAsyncResult* res, void* self);
  static void on_session_closed(XdpSession* session, void* self);

  GMainContext* ctx_ = nullptr;
  GMainLoop* loop_ = nullptr;
  XdpPortal* portal_ = nullptr;
  XdpSession* session_ = nullptr;
  Callback pending_;
  std::function<void()> closed_cb_;
  std::thread thread_;
};

}  // namespace tvlight
