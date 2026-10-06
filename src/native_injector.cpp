#include "native_injector.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#ifdef SKEY_HAVE_LIBEI
#include <libei.h>
#include <libportal/portal.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#endif

namespace skey {
struct NativeInjector::Impl {
    std::atomic<bool> ready{false}, stop{false};
    std::atomic<unsigned> generation{0};
    mutable std::mutex mutex;
    std::string message = "not started";
    std::thread worker;
    Display *x11Display = nullptr;
    std::mutex x11Mutex;
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
        // Write replacement token atomically; a killed Fcitx must not leave a
        // zero-length token that looks like a valid restore state.
        const auto tmp = path.string() + ".tmp-" + std::to_string(getpid());
        std::ofstream out(tmp, std::ios::trunc);
        if (out) {
            out << token << '\n';
            out.close();
            chmod(tmp.c_str(), 0600);
            std::filesystem::rename(tmp, path, ec);
            if (ec) std::filesystem::remove(tmp, ec);
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
        portal = xdp_portal_new();
        if (!portal) {
            fail("cannot create portal proxy");
        } else {
            pending = true;
            // Keyboard only: no screencast, pointer or clipboard access.
            // Ask the portal to persist its permission and restore it on the
            // next Fcitx restart when the compositor supports restore tokens.
#ifdef SKEY_LIBPORTAL_PERSIST
            loadToken();
            xdp_portal_create_remote_desktop_session_full(portal, XDP_DEVICE_KEYBOARD,
                static_cast<XdpOutputType>(0), XDP_REMOTE_DESKTOP_FLAG_NONE,
                XDP_CURSOR_MODE_HIDDEN, XDP_PERSIST_MODE_PERSISTENT,
                restoreToken.empty() ? nullptr : restoreToken.c_str(),
                cancellable, created, this);
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
        if (session) {
            // The portal consumes a restore token before starting a restored
            // session and writes it back only when Close is delivered. The
            // close call is asynchronous; give GLib time to flush it before
            // tearing down the portal connection during Fcitx shutdown.
            xdp_session_close(session);
            const gint64 deadline = g_get_monotonic_time() + 250000;
            while (g_get_monotonic_time() < deadline) {
                while (g_main_context_pending(context))
                    g_main_context_iteration(context, FALSE);
                g_usleep(10000);
            }
            g_object_unref(session);
        }
        if (portal) g_object_unref(portal);
        g_object_unref(cancellable);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }
#endif
};
NativeInjector::NativeInjector() : impl_(std::make_unique<Impl>()) {}
NativeInjector::~NativeInjector() {
    impl_->stop = true;
#ifdef SKEY_HAVE_LIBEI
    impl_->wake();
#endif
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->x11Display) {
        XCloseDisplay(impl_->x11Display);
        impl_->x11Display = nullptr;
    }
#ifdef SKEY_HAVE_LIBEI
    if (impl_->wakeFd >= 0) close(impl_->wakeFd);
#endif
}
void NativeInjector::start() {
    if (impl_->x11Display) return;
    const char *session = std::getenv("XDG_SESSION_TYPE");
    const char *display = std::getenv("DISPLAY");
    const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    const bool x11 = (session && std::string(session) == "x11") ||
                     ((!waylandDisplay || !*waylandDisplay) && display && *display);
    if (x11) {
        XInitThreads();
        impl_->x11Display = XOpenDisplay(nullptr);
        if (!impl_->x11Display) {
            impl_->status("cannot open X11 display");
            return;
        }
        int eventBase = 0, errorBase = 0, major = 0, minor = 0;
        if (!XTestQueryExtension(impl_->x11Display, &eventBase, &errorBase,
                                 &major, &minor)) {
            XCloseDisplay(impl_->x11Display);
            impl_->x11Display = nullptr;
            impl_->status("XTest extension unavailable");
            return;
        }
        impl_->ready = true;
        impl_->status("ready (XTest)");
        return;
    }
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
bool NativeInjector::ready() const { return impl_->ready; }
std::string NativeInjector::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->message;
}
void NativeInjector::cancel() { ++impl_->generation; }
bool NativeInjector::backspaces(int count, bool escape, unsigned pace) {
    if (impl_->x11Display) {
        std::lock_guard<std::mutex> lock(impl_->x11Mutex);
        KeyCode backspace = XKeysymToKeycode(impl_->x11Display, XK_BackSpace);
        KeyCode escapeKey = XKeysymToKeycode(impl_->x11Display, XK_Escape);
        if (!backspace || (escape && !escapeKey) || count < 0 || count > 64)
            return false;
        if (escape) {
            XTestFakeKeyEvent(impl_->x11Display, escapeKey, True, 0);
            XTestFakeKeyEvent(impl_->x11Display, escapeKey, False, 0);
            XFlush(impl_->x11Display);
            usleep(std::min(pace, 100000u));
        }
        for (int i = 0; i < count; ++i) {
            XTestFakeKeyEvent(impl_->x11Display, backspace, True, 0);
            XTestFakeKeyEvent(impl_->x11Display, backspace, False, 0);
            XFlush(impl_->x11Display);
            if (i + 1 < count) usleep(std::min(pace, 100000u));
        }
        return true;
    }
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
