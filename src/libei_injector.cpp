#include "libei_injector.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#ifdef SKEY_HAVE_LIBEI
#include <libei.h>
#include <libportal/portal.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace skey {
struct LibeiInjector::Impl {
    std::atomic<bool> ready{false}, stop{false};
    std::atomic<unsigned> generation{0};
    mutable std::mutex mutex;
    std::string message = "not started";
    std::thread worker;
    void status(const std::string &s) { std::lock_guard<std::mutex> lock(mutex); message = s; }
#ifdef SKEY_HAVE_LIBEI
    int wakeFd = -1;
    struct Batch { int count = 0; bool escape = false; unsigned pace = 1000, generation = 0; gint64 deadline = 0; } batch;
    GMainContext *context = nullptr;
    GCancellable *cancellable = nullptr;
    XdpPortal *portal = nullptr;
    XdpSession *session = nullptr;
    std::string restoreToken;
    ei *connection = nullptr;
    ei_device *keyboard = nullptr;
    GSource *inputSource = nullptr, *wakeSource = nullptr;
    bool pending = false;
    uint32_t sequence = 0;
    static std::filesystem::path tokenPath() {
        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        const char *home = std::getenv("HOME");
        std::filesystem::path base = xdg && *xdg ? xdg :
            (home && *home ? std::filesystem::path(home) / ".config" : ".");
        return base / "fcitx5" / "skey-libei-restore-token";
    }
    void loadToken() {
        std::ifstream in(tokenPath());
        std::getline(in, restoreToken);
    }
    void saveToken() {
        char *token = xdp_session_get_restore_token(session);
        if (!token || !*token) { g_free(token); return; }
        const auto path = tokenPath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        if (out) {
            out << token << '\n';
            out.close();
            chmod(path.c_str(), 0600);
        }
        g_free(token);
    }
    void wake() { uint64_t one = 1; if (wakeFd >= 0) (void)!write(wakeFd, &one, sizeof(one)); }
    void fail(const std::string &s) { ready = false; ++generation; status(s); }
    static void started(GObject *object, GAsyncResult *result, gpointer data) {
        auto &self = *static_cast<Impl *>(data);
        self.pending = false;
        GError *error = nullptr;
        if (!xdp_session_start_finish(XDP_SESSION(object), result, &error)) {
            self.fail(error ? error->message : "portal start failed");
            g_clear_error(&error); return;
        }
        self.saveToken();
        if (self.stop) return;
        if (!(xdp_session_get_devices(self.session) & XDP_DEVICE_KEYBOARD)) {
            self.fail("portal did not grant keyboard access"); return;
        }
        const int fd = xdp_session_connect_to_eis(self.session, &error);
        if (fd < 0) {
            self.fail(error ? error->message : "portal has no ConnectToEIS");
            g_clear_error(&error); return;
        }
        self.connection = ei_new_sender(&self);
        if (!self.connection) { close(fd); self.fail("ei_new_sender failed"); return; }
        ei_configure_name(self.connection, "SKey Libei experimental");
        if (ei_setup_backend_fd(self.connection, fd) < 0) {
            self.fail("EI handshake setup failed"); return;
        }
        self.inputSource = g_unix_fd_source_new(ei_get_fd(self.connection),
            static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP));
        g_source_set_callback(self.inputSource, G_SOURCE_FUNC(input), &self, nullptr);
        g_source_attach(self.inputSource, self.context);
        self.status("waiting for EIS keyboard");
        input(0, G_IO_IN, &self);
    }
    static void created(GObject *object, GAsyncResult *result, gpointer data) {
        auto &self = *static_cast<Impl *>(data);
        self.pending = false;
        GError *error = nullptr;
        self.session = xdp_portal_create_remote_desktop_session_finish(XDP_PORTAL(object), result, &error);
        if (!self.session) {
            self.fail(error ? error->message : "portal create failed");
            g_clear_error(&error); return;
        }
        g_signal_connect(self.session, "closed", G_CALLBACK(closed), &self);
        if (self.stop) return;
        self.pending = true;
        self.status("waiting for keyboard permission");
        xdp_session_start(self.session, nullptr, self.cancellable, started, &self);
    }
    static void closed(XdpSession *, gpointer data) {
        static_cast<Impl *>(data)->fail("portal session closed; restart Fcitx to retry");
    }
    static gboolean input(gint, GIOCondition condition, gpointer data) {
        auto &self = *static_cast<Impl *>(data);
        ei_dispatch(self.connection);
        while (auto *event = ei_get_event(self.connection)) {
            const auto type = ei_event_get_type(event);
            auto *device = ei_event_get_device(event);
            if (type == EI_EVENT_SEAT_ADDED) {
                ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD, nullptr);
            } else if (type == EI_EVENT_DEVICE_RESUMED && !self.keyboard &&
                       ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
                self.keyboard = ei_device_ref(device);
                ei_device_start_emulating(device, ++self.sequence);
                self.ready = true;
                self.status("ready");
            } else if ((type == EI_EVENT_DEVICE_PAUSED || type == EI_EVENT_DEVICE_REMOVED) &&
                       device == self.keyboard) {
                self.fail("EIS keyboard paused/removed");
                self.keyboard = ei_device_unref(self.keyboard);
            } else if (type == EI_EVENT_DISCONNECT) {
                self.fail("EIS disconnected; restart Fcitx to retry");
            }
            ei_event_unref(event);
        }
        if (condition & (G_IO_ERR | G_IO_HUP)) {
            self.fail("EIS connection closed"); return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }
    static gboolean commands(gint, GIOCondition, gpointer data) {
        auto &self = *static_cast<Impl *>(data);
        uint64_t value; (void)!read(self.wakeFd, &value, sizeof(value));
        if (self.stop) { g_cancellable_cancel(self.cancellable); return G_SOURCE_CONTINUE; }
        Batch b;
        { std::lock_guard<std::mutex> lock(self.mutex); b = self.batch; self.batch = {}; }
        if (!b.count && !b.escape) return G_SOURCE_CONTINUE;
        if (!self.ready || b.generation != self.generation || g_get_monotonic_time() > b.deadline)
            return G_SOURCE_CONTINUE;
        auto tap = [&](uint32_t key) {
            ei_device_keyboard_key(self.keyboard, key, true);
            ei_device_frame(self.keyboard, ei_now(self.connection));
            ei_device_keyboard_key(self.keyboard, key, false);
            ei_device_frame(self.keyboard, ei_now(self.connection));
            ei_dispatch(self.connection);
        };
        if (b.escape) { tap(KEY_ESC); g_usleep(b.pace); }
        for (int i = 0; i < b.count; ++i) {
            if (self.stop || b.generation != self.generation || !self.ready) break;
            tap(KEY_BACKSPACE);
            if (i + 1 < b.count) g_usleep(b.pace);
        }
        return G_SOURCE_CONTINUE;
    }
    void run() {
        context = g_main_context_new();
        g_main_context_push_thread_default(context);
        cancellable = g_cancellable_new();
        wakeSource = g_unix_fd_source_new(wakeFd, G_IO_IN);
        g_source_set_callback(wakeSource, G_SOURCE_FUNC(commands), this, nullptr);
        g_source_attach(wakeSource, context);
        // Do not use xdp_portal_initable_new() here.  That constructor
        // validates every portal interface, including ScreenCast.  A
        // keyboard-only Libei session only needs RemoteDesktop/EIS, and
        // some Ubuntu portal backends do not expose ScreenCast at all.
        portal = xdp_portal_new();
        if (!portal) {
            fail("cannot create portal proxy");
        } else {
            pending = true;
            // Keyboard only: no screencast, pointer or clipboard access.
            // Ask the portal to persist its permission and restore it on the
            // next Fcitx restart when the compositor supports restore tokens.
#ifdef SKEY_LIBPORTAL_PERSIST
            // The full API asks libportal to coordinate with ScreenCast on
            // some X11 portal implementations, which then fail with
            // InvalidArgs when ScreenCast is not exported.  X11 does not
            // need the restore-token path; use the basic RemoteDesktop API
            // there and keep persistence for native Wayland sessions.
            const bool wayland = std::getenv("WAYLAND_DISPLAY") &&
                                 *std::getenv("WAYLAND_DISPLAY");
            if (wayland) {
                loadToken();
                xdp_portal_create_remote_desktop_session_full(portal, XDP_DEVICE_KEYBOARD,
                    static_cast<XdpOutputType>(0), XDP_REMOTE_DESKTOP_FLAG_NONE,
                    XDP_CURSOR_MODE_HIDDEN, XDP_PERSIST_MODE_PERSISTENT,
                    restoreToken.empty() ? nullptr : restoreToken.c_str(),
                    cancellable, created, this);
            } else {
                status("requesting keyboard permission (X11 session)");
                xdp_portal_create_remote_desktop_session(portal, XDP_DEVICE_KEYBOARD,
                    static_cast<XdpOutputType>(0), XDP_REMOTE_DESKTOP_FLAG_NONE,
                    XDP_CURSOR_MODE_HIDDEN, cancellable, created, this);
            }
#else
            // libportal 0.7.1 supports EIS but not RemoteDesktop persistence.
            status("requesting keyboard permission (libportal < 0.8 cannot restore it)");
            xdp_portal_create_remote_desktop_session(portal, XDP_DEVICE_KEYBOARD,
                static_cast<XdpOutputType>(0), XDP_REMOTE_DESKTOP_FLAG_NONE,
                XDP_CURSOR_MODE_HIDDEN, cancellable, created, this);
#endif
            while (!stop || pending) g_main_context_iteration(context, TRUE);
        }
        ready = false;
        if (inputSource) { g_source_destroy(inputSource); g_source_unref(inputSource); }
        if (wakeSource) { g_source_destroy(wakeSource); g_source_unref(wakeSource); }
        if (keyboard) keyboard = ei_device_unref(keyboard);
        if (connection) connection = ei_unref(connection);
        if (session) { xdp_session_close(session); g_object_unref(session); }
        if (portal) g_object_unref(portal);
        g_object_unref(cancellable);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }
#endif
};
LibeiInjector::LibeiInjector() : impl_(std::make_unique<Impl>()) {}
LibeiInjector::~LibeiInjector() {
    impl_->stop = true;
#ifdef SKEY_HAVE_LIBEI
    impl_->wake();
#endif
    if (impl_->worker.joinable()) impl_->worker.join();
#ifdef SKEY_HAVE_LIBEI
    if (impl_->wakeFd >= 0) close(impl_->wakeFd);
#endif
}
void LibeiInjector::start() {
#ifdef SKEY_HAVE_LIBEI
    if (impl_->worker.joinable()) return;
    impl_->wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl_->wakeFd < 0) { impl_->status("eventfd failed"); return; }
    impl_->status("connecting to portal");
    impl_->worker = std::thread([this] { impl_->run(); });
#else
    impl_->status("built without libei/libportal development libraries");
#endif
}
bool LibeiInjector::ready() const { return impl_->ready; }
std::string LibeiInjector::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->message;
}
void LibeiInjector::cancel() { ++impl_->generation; }
bool LibeiInjector::backspaces(int count, bool escape, unsigned pace) {
#ifdef SKEY_HAVE_LIBEI
    if (!ready() || count < 0 || count > 64) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->batch.count || impl_->batch.escape) return false;
        impl_->batch = {count, escape, std::min(pace, 100000u),
                        impl_->generation.load(), g_get_monotonic_time() + 100000};
    }
    impl_->wake(); return true;
#else
    (void)count; (void)escape; (void)pace; return false;
#endif
}
}
