#include "portal.h"

#include <gio/gio.h>
#include <libportal/portal.h>

#include <iostream>

namespace tvlight {
namespace {
constexpr const char* kAppId = "io.github.kylaro.TvLight";
constexpr int kRegistryTimeoutMs = 2000;

// Host (non-Flatpak) apps can register an app id so the portal can remember
// the "allow" choice. Older portals lack the interface; that is fine.
void register_host_app() {
  GError* err = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
  if (!bus) {
    std::cerr << "portal: no session bus: " << (err ? err->message : "?") << "\n";
    g_clear_error(&err);
    return;
  }
  GVariant* reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.host.portal.Registry",
      "Register", g_variant_new("(sa{sv})", kAppId, nullptr), nullptr, G_DBUS_CALL_FLAGS_NONE, kRegistryTimeoutMs,
      nullptr, &err);
  if (reply) {
    g_variant_unref(reply);
  } else {
    std::cerr << "portal: app id registration unavailable (" << err->message << ")\n";
    g_clear_error(&err);
  }
  g_object_unref(bus);
}
}  // namespace

Portal::Portal() {
  ctx_ = g_main_context_new();
  loop_ = g_main_loop_new(ctx_, FALSE);
  thread_ = std::thread([this] {
    g_main_context_push_thread_default(ctx_);
    g_main_loop_run(loop_);
    g_main_context_pop_thread_default(ctx_);
  });
}

Portal::~Portal() {
  run_on_loop([this] {
    if (session_) {
      xdp_session_close(session_);
      g_clear_object(&session_);
    }
    g_clear_object(&portal_);
    g_main_loop_quit(loop_);
  });
  if (thread_.joinable()) thread_.join();
  g_main_loop_unref(loop_);
  g_main_context_unref(ctx_);
}

void Portal::run_on_loop(std::function<void()> fn) {
  auto* heap = new std::function<void()>(std::move(fn));
  g_main_context_invoke_full(
      ctx_, G_PRIORITY_DEFAULT,
      [](gpointer p) -> gboolean {
        (*static_cast<std::function<void()>*>(p))();
        return G_SOURCE_REMOVE;
      },
      heap, [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
}

void Portal::request(std::string restore_token, Callback done) {
  run_on_loop([this, token = std::move(restore_token), done = std::move(done)]() mutable {
    pending_ = std::move(done);
    do_request(token);
  });
}

void Portal::close() {
  run_on_loop([this] {
    if (session_) {
      g_signal_handlers_disconnect_by_data(session_, this);
      xdp_session_close(session_);
      g_clear_object(&session_);
    }
  });
}

void Portal::do_request(const std::string& token) {
  if (!portal_) {
    register_host_app();
    GError* err = nullptr;
    portal_ = xdp_portal_initable_new(&err);
    if (!portal_) {
      PortalResult r;
      r.error = std::string("portal unavailable: ") + (err ? err->message : "?");
      g_clear_error(&err);
      finish(std::move(r));
      return;
    }
  }
  if (session_) {
    g_signal_handlers_disconnect_by_data(session_, this);
    xdp_session_close(session_);
    g_clear_object(&session_);
  }
  xdp_portal_create_screencast_session(portal_, XDP_OUTPUT_MONITOR, XDP_SCREENCAST_FLAG_NONE, XDP_CURSOR_MODE_HIDDEN,
                                       XDP_PERSIST_MODE_PERSISTENT, token.empty() ? nullptr : token.c_str(), nullptr,
                                       reinterpret_cast<GAsyncReadyCallback>(&Portal::on_session_created), this);
}

void Portal::on_session_created(GObject*, GAsyncResult* res, void* self_ptr) {
  auto* self = static_cast<Portal*>(self_ptr);
  GError* err = nullptr;
  XdpSession* session = xdp_portal_create_screencast_session_finish(self->portal_, res, &err);
  if (!session) {
    PortalResult r;
    r.cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    r.error = std::string("create session failed: ") + (err ? err->message : "?");
    g_clear_error(&err);
    self->finish(std::move(r));
    return;
  }
  self->session_ = session;
  g_signal_connect(session, "closed", G_CALLBACK(&Portal::on_session_closed), self);
  xdp_session_start(session, nullptr, nullptr, reinterpret_cast<GAsyncReadyCallback>(&Portal::on_session_started),
                    self);
}

void Portal::on_session_started(GObject*, GAsyncResult* res, void* self_ptr) {
  auto* self = static_cast<Portal*>(self_ptr);
  PortalResult r;
  GError* err = nullptr;
  if (!self->session_ || !xdp_session_start_finish(self->session_, res, &err)) {
    r.cancelled = err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    r.error = std::string("start failed: ") + (err ? err->message : "session gone");
    g_clear_error(&err);
    self->finish(std::move(r));
    return;
  }

  GVariant* streams = xdp_session_get_streams(self->session_);
  if (streams && g_variant_n_children(streams) > 0) {
    GVariant* props = nullptr;
    g_variant_get_child(streams, 0, "(u@a{sv})", &r.node_id, &props);
    g_variant_lookup(props, "size", "(ii)", &r.width, &r.height);
    g_variant_unref(props);
  } else {
    r.error = "portal returned no streams";
    self->finish(std::move(r));
    return;
  }
  r.pipewire_fd = xdp_session_open_pipewire_remote(self->session_);
  if (char* token = xdp_session_get_restore_token(self->session_)) {
    r.restore_token = token;
    g_free(token);
  }
  r.ok = r.pipewire_fd >= 0;
  if (!r.ok) r.error = "could not open PipeWire remote";
  self->finish(std::move(r));
}

void Portal::on_session_closed(XdpSession*, void* self_ptr) {
  auto* self = static_cast<Portal*>(self_ptr);
  std::cerr << "portal: session closed by compositor\n";
  if (self->closed_cb_) self->closed_cb_();
}

void Portal::finish(PortalResult r) {
  Callback cb = std::move(pending_);
  pending_ = nullptr;
  if (cb) cb(std::move(r));
}

}  // namespace tvlight
