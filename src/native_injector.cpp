#include "native_injector.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <X11/XKBlib.h>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#ifdef SKEY_HAVE_LIBEI
#include <libei.h>
// Public libportal umbrella; its component headers provide the declarations.
#include <libportal/portal.h> // IWYU pragma: keep
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#endif

namespace skey {
namespace {
class XTestTransport final : public NativeInjector::Transport {
    Display *display = nullptr;
public:
    bool open(std::string &error) override {
        display = XOpenDisplay(nullptr);
        if (!display) { error = "cannot open X11 display"; return false; }
        int event, code, major, minor;
        if (!XTestQueryExtension(display, &event, &code, &major, &minor)) {
            error = "XTest extension unavailable"; close(); return false;
        }
        return true;
    }
    unsigned keycode(unsigned long symbol) override { return XKeysymToKeycode(display, symbol); }
    bool modifiers(unsigned &mask) override {
        XkbStateRec state{};
        if (XkbGetState(display, XkbUseCoreKbd, &state) != Success) return false;
        mask = state.mods; return true;
    }
    bool key(unsigned code, bool down) override { return XTestFakeKeyEvent(display, code, down, 0); }
    bool focusWindow(uint64_t &window) override {
        Window focus = None;
        int revert = 0;
        if (!XGetInputFocus(display, &focus, &revert)) return false;
        window = focus;
        // PointerRoot follows the pointer and is not a stable batch target.
        return focus != None && focus != PointerRoot;
    }
    bool flush() override { XFlush(display); return true; }
    void close() override { if (display) XCloseDisplay(display); display = nullptr; }
};
}

struct NativeInjector::Impl {
    std::atomic<bool> ready{false}, stop{false};
    std::atomic<unsigned> generation{0};
    mutable std::mutex mutex;
    std::string message = "not started";
    std::thread worker;
    std::unique_ptr<Transport> transport;
    bool injectedTransport = false;
    std::atomic<bool> x11{false};
    std::mutex queueMutex;
    std::condition_variable changed;
    struct Work {
        Owner owner;
        int count;
        bool escape, select;
        unsigned pace;
        uint64_t expectedWindow;
        Ticket ticket;
        std::atomic<bool> cancelled{false};
    };
    std::deque<std::shared_ptr<Work>> queue;
    std::shared_ptr<Work> active;
    uint64_t nextId = 1;
    Ticket submit(Owner owner, int count, bool escape, bool select, unsigned pace,
                  uint64_t expectedWindow = 0) {
        if (!x11 || !ready || count < 0 || count > 64) return {};
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stop || queue.size() >= 64) return {};
        auto work = std::make_shared<Work>();
        work->owner = owner; work->count = count; work->escape = escape;
        work->select = select; work->pace = std::min(pace, 100000u);
        work->expectedWindow = expectedWindow;
        work->ticket = std::make_shared<Request>(); work->ticket->id = nextId++;
        queue.push_back(work); changed.notify_all(); return work->ticket;
    }
    void cancelX(Owner owner, bool all = false) {
        std::lock_guard<std::mutex> lock(queueMutex);
        for (auto it = queue.begin(); it != queue.end();) {
            if (all || (*it)->owner == owner) {
                (*it)->ticket->state = State::Cancelled; it = queue.erase(it);
            } else ++it;
        }
        if (active && (all || active->owner == owner)) active->cancelled = true;
        changed.notify_all();
    }
    void runX() {
        std::string error;
        if (!transport->open(error)) { status(error); transport->close(); return; }
        status("ready (XTest)"); ready = true;
        for (;;) {
            std::shared_ptr<Work> work;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                changed.wait(lock, [&] { return stop || !queue.empty(); });
                if (stop) break;
                work = queue.front(); queue.pop_front(); active = work;
                work->ticket->state = State::Running;
            }
            const auto cancelled = [&] { return stop || work->cancelled.load(); };
            unsigned held = 0;
            bool shiftHeld = false;
            uint64_t targetWindow = work->expectedWindow;
            const bool hasTarget = targetWindow || transport->focusWindow(targetWindow);
            const auto focusSafe = [&] {
                uint64_t currentWindow = 0;
                return hasTarget && transport->focusWindow(currentWindow) &&
                    currentWindow == targetWindow;
            };
            const unsigned shift = transport->keycode(XK_Shift_L);
            const unsigned key = transport->keycode(work->select ? XK_Left : XK_BackSpace);
            const unsigned escape = transport->keycode(XK_Escape);
            const auto modifiersSafe = [&] {
                unsigned mask = 0;
                return transport->modifiers(mask) &&
                    !(mask & ~(LockMask | Mod2Mask | (shiftHeld ? ShiftMask : 0)));
            };
            const auto emit = [&](unsigned code, bool down) {
                // An attempted transport write may have reached the server even
                // when it reports failure; never replay such a request.
                ++work->ticket->emittedEvents;
                return transport->key(code, down) && transport->flush();
            };
            const auto tap = [&](unsigned code) {
                if (cancelled() || !modifiersSafe() || !focusSafe()) return false;
                held = code;
                if (!emit(code, true)) return false;
                if (!emit(code, false)) return false;
                held = 0; return true;
            };
            const auto pause = [&] {
                std::unique_lock<std::mutex> lock(queueMutex);
                changed.wait_for(lock, std::chrono::microseconds(work->pace), cancelled);
                lock.unlock();
                return !cancelled() && focusSafe();
            };
            bool ok = !cancelled() && key && (!work->escape || escape) &&
                (!work->select || shift) && modifiersSafe() && focusSafe();
            if (ok && work->select) {
                shiftHeld = true;
                ok = emit(shift, true);
            }
            if (ok && work->escape) ok = tap(escape) && pause();
            const int taps = work->count;
            for (int i = 0; ok && i < taps; ++i) {
                ok = tap(key);
                if (ok && i + 1 < taps) ok = pause();
            }
            if (work->select && shiftHeld) {
                const bool released = emit(shift, false);
                if (released) shiftHeld = false;
                ok = released && ok;
                // The engine consumes this unshifted Left as the anchor.
                // Release Shift first so replayed input cannot observe it held.
                if (ok) ok = tap(key);
            }
            // Release only keys this batch attempted to press. Initial held
            // Shift was rejected; concurrent physical input is not atomic in X11.
            if (held) ok = emit(held, false) && ok;
            if (shiftHeld) ok = emit(shift, false) && ok;
            if (!ok && !cancelled()) {
                status("XTest request " + std::to_string(work->ticket->id) +
                    " failed (focus, keymap, modifier, or transport); attempted events=" +
                    std::to_string(work->ticket->emittedEvents.load()));
            }
            work->ticket->state = cancelled() ? State::Cancelled :
                (ok ? State::Complete : State::Failed);
            {
                std::lock_guard<std::mutex> lock(queueMutex); active.reset();
            }
        }
        ready = false; cancelX(0, true); transport->close();
    }
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
#ifdef SKEY_LIBPORTAL_PERSIST
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
#endif
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
#ifdef SKEY_LIBPORTAL_PERSIST
        self.saveToken();
#endif
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
        // xdp_portal_new() aborts the whole process (g_error) when the
        // session bus is unreachable — it took Fcitx down with it.  The
        // initable variant reports the failure instead (libportal >= 0.7).
        GError *portalError = nullptr;
        portal = xdp_portal_initable_new(&portalError);
        if (!portal) {
            fail(std::string("cannot create portal proxy: ") +
                 (portalError ? portalError->message : "unknown error"));
            g_clear_error(&portalError);
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
NativeInjector::NativeInjector(std::unique_ptr<Transport> transport) : NativeInjector() {
    impl_->transport = std::move(transport); impl_->injectedTransport = true;
}
NativeInjector::~NativeInjector() {
    impl_->stop = true;
    impl_->cancelX(0, true);
#ifdef SKEY_HAVE_LIBEI
    impl_->wake();
#endif
    if (impl_->worker.joinable()) impl_->worker.join();
#ifdef SKEY_HAVE_LIBEI
    if (impl_->wakeFd >= 0) close(impl_->wakeFd);
#endif
}
void NativeInjector::start() {
    if (impl_->worker.joinable()) return;
    const char *session = std::getenv("XDG_SESSION_TYPE");
    const char *display = std::getenv("DISPLAY");
    const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // Match the engine/settings policy even with inherited or contradictory
    // session variables: a Wayland display always excludes native X11.
    const bool x11 = display && *display &&
                     !(waylandDisplay && *waylandDisplay) &&
                     !(session && std::string(session) == "wayland");
    if (x11 || impl_->injectedTransport) {
        // Initialize before this backend starts using Xlib on its worker.
        // Fcitx's native X11 backend uses XCB, not this connection.
        if (!impl_->injectedTransport && !XInitThreads()) {
            impl_->status("cannot initialize Xlib threads"); return;
        }
        impl_->x11 = true;
        if (!impl_->transport) impl_->transport = std::make_unique<XTestTransport>();
        impl_->status("connecting to XTest");
        impl_->worker = std::thread([this] { impl_->runX(); });
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
void NativeInjector::restoreSession() {
#if defined(SKEY_HAVE_LIBEI) && defined(SKEY_LIBPORTAL_PERSIST)
    const char *session = std::getenv("XDG_SESSION_TYPE");
    const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (!session || std::string(session) != "wayland" || !desktop) return;
    std::string desktops = ":" + std::string(desktop) + ":";
    std::transform(desktops.begin(), desktops.end(), desktops.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (desktops.find(":kde:") == std::string::npos &&
        desktops.find(":kde-plasma:") == std::string::npos &&
        desktops.find(":plasma:") == std::string::npos) return;

    std::ifstream in(Impl::tokenPath());
    std::string token;
    if (!std::getline(in, token) || token.empty()) return;

    // KDE's portal owns a fake-input device with the touch seat capability,
    // even for keyboard-only sessions. Closing the old daemon removes it.
    // winit then destroys text-input-v3 on capability removal and recreates
    // it only on capability addition. Restore our persisted portal session
    // so Uinput commits reach text-input again instead of KWin's lossy
    // keysym fallback. Do not select Native or enqueue any input here.
    start();
#endif
}
bool NativeInjector::ready() const { return impl_->ready; }
std::string NativeInjector::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->message;
}
void NativeInjector::cancel() { ++impl_->generation; impl_->cancelX(0, true); }
void NativeInjector::cancel(Owner owner) { impl_->cancelX(owner); }
NativeInjector::Ticket NativeInjector::submitBackspaces(Owner owner, int count, bool escape, unsigned pace,
                                                       uint64_t expectedWindow) {
    return impl_->submit(owner, count, escape, false, pace, expectedWindow);
}
NativeInjector::Ticket NativeInjector::submitShiftSelect(Owner owner, int count, unsigned pace,
                                                        uint64_t expectedWindow) {
    return impl_->submit(owner, count, false, true, pace, expectedWindow);
}
bool NativeInjector::backspaces(int count, bool escape, unsigned pace) {
    if (impl_->x11) return static_cast<bool>(submitBackspaces(0, count, escape, pace));
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
