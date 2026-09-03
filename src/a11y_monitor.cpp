#include "a11y_monitor.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dbus/dbus.h>

// AT-SPI2 role constants (from atspi-constants.h)
static constexpr int ROLE_DOCUMENT_WEB = 95;
static constexpr int ROLE_DOCUMENT_FRAME = 82;
static constexpr int ROLE_PASSWORD_TEXT = 40;
// Chromium may emit a focus event for a nested accessibility node inside a
// contenteditable control. Facebook comments currently reach DOCUMENT_WEB at
// depth 22, so 20 incorrectly classifies that event as browser chrome.
static constexpr int MAX_ANCESTOR_DEPTH = 64;

// Debug logging — controlled by the debug_ atomic flag via a thread-local
// pointer. The thread function sets this up so A11Y_LOG can check it.
static thread_local const std::atomic<bool> *g_debugFlag = nullptr;

static FILE *logFile() {
    static FILE *f = nullptr;
    if (!f) f = fopen("/tmp/skey_a11y.log", "a");
    return f;
}

#define A11Y_LOG(fmt, ...)                                                     \
    do {                                                                        \
        if (g_debugFlag && g_debugFlag->load(std::memory_order_relaxed)) {      \
            FILE *f = logFile();                                                \
            if (f) {                                                            \
                struct timespec ts;                                             \
                clock_gettime(CLOCK_REALTIME, &ts);                             \
                struct tm tmv;                                                  \
                localtime_r(&ts.tv_sec, &tmv);                                  \
                fprintf(f, "[a11y %02d:%02d:%02d.%03ld] " fmt "\n",             \
                        tmv.tm_hour, tmv.tm_min, tmv.tm_sec,                    \
                        ts.tv_nsec / 1000000, ##__VA_ARGS__);                   \
                fflush(f);                                                      \
            }                                                                   \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// AT-SPI2 bus connection
// ---------------------------------------------------------------------------

static std::string getAtspiBusAddress() {
    const char *env = getenv("AT_SPI_BUS_ADDRESS");
    if (env && env[0]) return env;

    FILE *fp = popen(
        "xprop -root AT_SPI_BUS 2>/dev/null | cut -d'\"' -f2",
        "r");
    if (fp) {
        char buf[512] = {};
        if (fgets(buf, sizeof(buf), fp)) {
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                buf[--len] = '\0';
            pclose(fp);
            if (buf[0]) {
                A11Y_LOG("Bus address from X11: %s", buf);
                return buf;
            }
        } else {
            pclose(fp);
        }
    }

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *session = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (session && !dbus_error_is_set(&err)) {
        DBusMessage *msg = dbus_message_new_method_call(
            "org.a11y.atspi.Bus", "/org/a11y/atspi/bus",
            "org.a11y.atspi.Bus", "GetAddress");
        if (msg) {
            DBusMessage *reply = dbus_connection_send_with_reply_and_block(
                session, msg, 2000, &err);
            dbus_message_unref(msg);
            if (reply && !dbus_error_is_set(&err)) {
                const char *s = nullptr;
                if (dbus_message_get_args(reply, &err,
                                          DBUS_TYPE_STRING, &s,
                                          DBUS_TYPE_INVALID) &&
                    s && s[0]) {
                    std::string addr = s;
                    dbus_message_unref(reply);
                    dbus_error_free(&err);
                    dbus_connection_unref(session);
                    return addr;
                }
                if (reply) dbus_message_unref(reply);
            }
        }
        dbus_error_free(&err);
        dbus_connection_unref(session);
    } else {
        dbus_error_free(&err);
    }

    return {};
}

static DBusConnection *connectAtspiBus() {
    DBusError err;
    dbus_error_init(&err);
    std::string addr = getAtspiBusAddress();
    DBusConnection *bus = nullptr;

    if (!addr.empty()) {
        bus = dbus_connection_open(addr.c_str(), &err);
        if (bus && !dbus_error_is_set(&err)) {
            if (dbus_bus_register(bus, &err) && !dbus_error_is_set(&err)) {
                A11Y_LOG("Connected to AT-SPI2 bus");
                dbus_error_free(&err);
                return bus;
            }
            dbus_connection_unref(bus);
        }
        dbus_error_free(&err);
        dbus_error_init(&err);
    }

    bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (bus && !dbus_error_is_set(&err)) {
        A11Y_LOG("Connected to session bus (fallback)");
        dbus_error_free(&err);
        return bus;
    }

    dbus_error_free(&err);
    return nullptr;
}

// ---------------------------------------------------------------------------
// AT-SPI2 accessible queries
// ---------------------------------------------------------------------------

static int queryProcessId(DBusConnection *bus, const char *sender,
                          const char *path) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.a11y.atspi.Accessible", "GetProcessId");
    if (!msg) return -1;

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 500, &err);
    dbus_message_unref(msg);

    int pid = -1;
    if (reply && !dbus_error_is_set(&err)) {
        dbus_int32_t p = -1;
        if (dbus_message_get_args(reply, &err, DBUS_TYPE_INT32, &p,
                                  DBUS_TYPE_INVALID))
            pid = static_cast<int>(p);
        dbus_message_unref(reply);
    }
    dbus_error_free(&err);
    return pid;
}

// Resolve the pid of the focused app's AT-SPI connection via the a11y
// registry (org.a11y.Bus on the session bus) instead of round-tripping
// to the app.  GetProcessId on browser-UI elements can stall AT-SPI,
// and non-web apps (which skip GetProcessId) otherwise leave pid=-1,
// forcing the engine into full /proc scans — which can misclassify.
// The registry answers from its own bookkeeping, so it cannot stall.
static int queryConnectionPid(const char *sender) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *session = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!session || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return -1;
    }
    DBusMessage *msg = dbus_message_new_method_call(
        "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus",
        "GetConnectionUnixProcessID");
    if (!msg) {
        dbus_connection_unref(session);
        return -1;
    }
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &sender,
                             DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        session, msg, 500, &err);
    dbus_message_unref(msg);

    int pid = -1;
    if (reply && !dbus_error_is_set(&err)) {
        dbus_int32_t p = -1;
        if (dbus_message_get_args(reply, &err, DBUS_TYPE_INT32, &p,
                                  DBUS_TYPE_INVALID))
            pid = static_cast<int>(p);
        dbus_message_unref(reply);
    }
    dbus_error_free(&err);
    dbus_connection_unref(session);
    return pid;
}

static int queryRole(DBusConnection *bus, const char *sender,
                     const char *path) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.a11y.atspi.Accessible", "GetRole");
    if (!msg) return -1;

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 500, &err);
    dbus_message_unref(msg);

    int role = -1;
    if (reply && !dbus_error_is_set(&err)) {
        dbus_uint32_t r = 0;
        if (dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32, &r,
                                  DBUS_TYPE_INVALID))
            role = static_cast<int>(r);
        dbus_message_unref(reply);
    }
    dbus_error_free(&err);
    return role;
}

// ── Read the accessible text of the focused entry (snapshot polling) ──
static std::string queryText(DBusConnection *bus, const char *sender,
                             const char *path) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.a11y.atspi.Text", "GetText");
    if (!msg) return {};
    dbus_int32_t start = 0, end = -1;
    dbus_message_append_args(msg, DBUS_TYPE_INT32, &start, DBUS_TYPE_INT32,
                             &end, DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 500, &err);
    dbus_message_unref(msg);
    std::string text;
    if (reply && !dbus_error_is_set(&err)) {
        DBusMessageIter iter;
        if (dbus_message_iter_init(reply, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
            const char *s = nullptr;
            dbus_message_iter_get_basic(&iter, &s);
            if (s) text = s;
        }
        dbus_message_unref(reply);
    }
    dbus_error_free(&err);
    return text;
}

static bool querySelection(DBusConnection *bus, const char *sender,
                           const char *path, int &selStart, int &selEnd) {
    selStart = -1;
    selEnd = -1;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.a11y.atspi.Text", "GetSelection");
    if (!msg) {
        dbus_error_free(&err);
        return false;
    }
    dbus_int32_t selNum = 0;
    dbus_message_append_args(msg, DBUS_TYPE_INT32, &selNum, DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 500, &err);
    dbus_message_unref(msg);
    if (reply && !dbus_error_is_set(&err)) {
        // Reply may be a struct (i, i) directly or wrapped in a variant.
        DBusMessageIter rit, var, st;
        DBusMessageIter *cur = &rit;
        if (dbus_message_iter_init(reply, &rit)) {
            if (dbus_message_iter_get_arg_type(cur) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(cur, &var);
                cur = &var;
            }
            if (dbus_message_iter_get_arg_type(cur) == DBUS_TYPE_STRUCT) {
                dbus_message_iter_recurse(cur, &st);
                dbus_int32_t v1 = -1, v2 = -1;
                if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT32) {
                    dbus_message_iter_get_basic(&st, &v1);
                    dbus_message_iter_next(&st);
                    if (dbus_message_iter_get_arg_type(&st) ==
                        DBUS_TYPE_INT32)
                        dbus_message_iter_get_basic(&st, &v2);
                }
                if (v1 >= 0 && v2 > v1) {
                    selStart = static_cast<int>(v1);
                    selEnd = static_cast<int>(v2);
                }
            }
        }
        dbus_message_unref(reply);
        return true;
    }
    if (reply)
        dbus_message_unref(reply);
    dbus_error_free(&err);
    return false;
}

static bool queryParent(DBusConnection *bus, const char *sender,
                        const char *path,
                        std::string &outSender, std::string &outPath) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return false;

    const char *iface = "org.a11y.atspi.Accessible";
    const char *prop = "Parent";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
                             DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 500, &err);
    dbus_message_unref(msg);

    if (!reply || dbus_error_is_set(&err)) {
        if (reply) dbus_message_unref(reply);
        dbus_error_free(&err);
        return false;
    }

    DBusMessageIter iter, variant, struc;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
        dbus_message_unref(reply);
        return false;
    }
    dbus_message_iter_recurse(&iter, &variant);
    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_STRUCT) {
        dbus_message_unref(reply);
        return false;
    }
    dbus_message_iter_recurse(&variant, &struc);

    const char *parentBus = nullptr;
    const char *parentPath = nullptr;
    if (dbus_message_iter_get_arg_type(&struc) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&struc, &parentBus);
        dbus_message_iter_next(&struc);
        if (dbus_message_iter_get_arg_type(&struc) == DBUS_TYPE_OBJECT_PATH)
            dbus_message_iter_get_basic(&struc, &parentPath);
    }

    bool ok = false;
    if (parentBus && parentPath && parentPath[0] == '/') {
        outSender = parentBus;
        outPath = parentPath;
        ok = true;
    }
    dbus_message_unref(reply);
    return ok;
}

// Human-readable names for AT-SPI2 roles observed in practice (values from
// atspi-constants.h ATSPI_ROLE_*).  Used for debug logging only.
static const char *roleName(int role) {
    switch (role) {
    case 11: return "combo_box";
    case 20: return "filler";
    case 23: return "frame";
    case 30: return "layered_pane";
    case 31: return "list";
    case 32: return "list_item";
    case 35: return "menu_item";
    case 37: return "page_tab";
    case 39: return "panel";
    case 40: return "password_text";
    case 41: return "popup_menu";
    case 43: return "button";
    case 55: return "table";
    case 56: return "table_cell";
    case 61: return "text";
    case 73: return "paragraph";
    case 79: return "entry";
    case 82: return "document_frame";
    case 85: return "section";
    case 94: return "document_text";
    case 95: return "document_web";
    case 110: return "description_list";
    default: return "?";
    }
}

static bool hasDocumentWebAncestor(DBusConnection *bus,
                                   const char *sender,
                                   const char *path) {
    std::string curSender = sender;
    std::string curPath = path;

    for (int depth = 0; depth < MAX_ANCESTOR_DEPTH; ++depth) {
        std::string parentSender, parentPath;
        if (!queryParent(bus, curSender.c_str(), curPath.c_str(),
                         parentSender, parentPath))
            break;

        if (parentPath == "/org/a11y/atspi/null" ||
            parentPath == "/org/a11y/atspi/accessible/root")
            break;

        int role = queryRole(bus, parentSender.c_str(), parentPath.c_str());
        A11Y_LOG("  ancestor[%d]: role=%d path=%s", depth, role,
                 parentPath.c_str());
        if (role == ROLE_DOCUMENT_WEB || role == ROLE_DOCUMENT_FRAME)
            return true;

        curSender = parentSender;
        curPath = parentPath;
    }
    return false;
}

// Query the focused node's state set.  org.a11y.atspi.Accessible.GetState
// returns an array of two uint32 — low and high halves of the ATSPI_STATE_*
// bitmask (atspi-constants.h).  Fills the interesting flags and a debug
// string of the set states.
static bool queryStates(DBusConnection *bus, const char *sender,
                        const char *path, bool &editable, bool &multiline,
                        bool &singleLine, std::string &outNames) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.a11y.atspi.Accessible", "GetState");
    if (!msg) return false;

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 500, &err);
    dbus_message_unref(msg);

    if (!reply || dbus_error_is_set(&err)) {
        if (reply) dbus_message_unref(reply);
        dbus_error_free(&err);
        return false;
    }

    uint64_t states = 0;
    DBusMessageIter iter, arr;
    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&iter, &arr);
        int shift = 0;
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_UINT32 &&
               shift < 64) {
            dbus_uint32_t v = 0;
            dbus_message_iter_get_basic(&arr, &v);
            states |= static_cast<uint64_t>(v) << shift;
            shift += 32;
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(reply);
    dbus_error_free(&err);

    // ATSPI_STATE_* bit positions (see atspi-constants.h enum order).
    static constexpr struct { int bit; const char *name; } kStateBits[] = {
        {6, "editable"},  {7, "enabled"},   {10, "focusable"},
        {11, "focused"},  {16, "multi-line"}, {17, "multiselectable"},
        {23, "sensitive"}, {24, "showing"}, {25, "single-line"},
        {30, "manages_descendants"},
    };
    editable = false;
    multiline = false;
    singleLine = false;
    outNames.clear();
    for (const auto &sb : kStateBits) {
        if (states & (1ULL << sb.bit)) {
            if (!outNames.empty()) outNames += ",";
            outNames += sb.name;
            if (sb.bit == 6) editable = true;
            if (sb.bit == 16) multiline = true;
            if (sb.bit == 25) singleLine = true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Chromium native accessibility activation
// ---------------------------------------------------------------------------
// Chromium (>= ~M12x, verified against 150) no longer reads
// org.a11y.Status.ScreenReaderEnabled. After a restart its accessible tree
// stays empty (no focus events for the address bar) until an AT-SPI client
// calls GetRelationSet or GetAttributes on one of its objects — Chromium
// treats those calls as "a screen reader is exploring me" and enables native
// accessibility for the rest of the browser session (AtkRefRelationSet in
// ui/accessibility/platform/ax_platform_node_auralinux.cc). Poking the app
// root of every Chromium-based browser on the bus replaces having to enable
// chrome://accessibility manually after each browser restart.

static std::string queryName(DBusConnection *bus, const char *sender,
                             const char *path) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return {};

    const char *iface = "org.a11y.atspi.Accessible";
    const char *prop = "Name";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
                             DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 500, &err);
    dbus_message_unref(msg);

    std::string name;
    if (reply && !dbus_error_is_set(&err)) {
        DBusMessageIter iter, variant;
        if (dbus_message_iter_init(reply, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&iter, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
                const char *s = nullptr;
                dbus_message_iter_get_basic(&variant, &s);
                if (s) name = s;
            }
        }
    }
    if (reply) dbus_message_unref(reply);
    dbus_error_free(&err);
    return name;
}

static void pokeA11yApps(DBusConnection *bus) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        "org.a11y.atspi.Registry", "/org/a11y/atspi/accessible/root",
        "org.a11y.atspi.Accessible", "GetChildren");
    if (!msg) return;

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 2000, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) {
        if (reply) dbus_message_unref(reply);
        dbus_error_free(&err);
        return;
    }

    DBusMessageIter iter, arr;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return;
    }
    dbus_message_iter_recurse(&iter, &arr);

    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
        DBusMessageIter struc;
        dbus_message_iter_recurse(&arr, &struc);

        const char *appBus = nullptr;
        const char *appPath = nullptr;
        if (dbus_message_iter_get_arg_type(&struc) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&struc, &appBus);
            dbus_message_iter_next(&struc);
            if (dbus_message_iter_get_arg_type(&struc) == DBUS_TYPE_OBJECT_PATH)
                dbus_message_iter_get_basic(&struc, &appPath);
        }

        if (appBus && appPath && appPath[0] == '/') {
            // Poke EVERY app, not just browser names: Electron apps
            // (antigravity-ide, VS Code forks) re-enable Chromium's
            // native accessibility on the same GetRelationSet/GetAttributes
            // trigger as Chrome, but their AT-SPI names don't match the
            // browser list — and after an fcitx5 restart their a11y tree
            // stays dead otherwise (no focus events for the integrated
            // terminal).  The query is read-only and cheap; non-Chromium
            // apps just return an empty relation set.
            std::string name = queryName(bus, appBus, appPath);
            DBusMessage *poke = dbus_message_new_method_call(
                appBus, appPath, "org.a11y.atspi.Accessible",
                "GetRelationSet");
            if (poke) {
                DBusError perr;
                dbus_error_init(&perr);
                DBusMessage *preply =
                    dbus_connection_send_with_reply_and_block(
                        bus, poke, 500, &perr);
                if (preply) dbus_message_unref(preply);
                dbus_message_unref(poke);
                A11Y_LOG("Poked '%s' (%s) to enable native a11y%s",
                         name.c_str(), appBus,
                         dbus_error_is_set(&perr) ? " [failed]" : "");
                dbus_error_free(&perr);
            }
        }
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(reply);
    dbus_error_free(&err);
}

// ---------------------------------------------------------------------------
// A11yMonitor
// ---------------------------------------------------------------------------

A11yMonitor::A11yMonitor() = default;

A11yMonitor::~A11yMonitor() { stop(); }

std::string A11yMonitor::atspiBusAddress() { return getAtspiBusAddress(); }

bool A11yMonitor::focusedTextEntry(std::string &busName, std::string &path,
                                   uint64_t &snapshotUsec) const {
    std::lock_guard<std::mutex> lock(focusEntryMutex_);
    if (focusEntryBus_.empty() || focusEntryPath_.empty())
        return false;
    busName = focusEntryBus_;
    path = focusEntryPath_;
    snapshotUsec = focusEntrySnapshotUsec_.load(std::memory_order_relaxed);
    return true;
}

bool A11yMonitor::a11yState(std::string &text, int &selStart, int &selEnd,
                            uint64_t maxAgeUsec) const {
    std::lock_guard<std::mutex> lock(a11ySnapshotMutex_);
    if (a11ySnapshotUsec_ == 0)
        return false;
    uint64_t nowUsec = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() / 1000);
    if (nowUsec - a11ySnapshotUsec_ > maxAgeUsec)
        return false;
    text = a11ySnapshotText_;
    selStart = a11ySnapshotSelStart_;
    selEnd = a11ySnapshotSelEnd_;
    return true;
}

void A11yMonitor::waitForSnapshotUpdate(uint64_t timeoutUsec) const {
    uint64_t snap;
    {
        std::lock_guard<std::mutex> lock(a11ySnapshotMutex_);
        snap = a11ySnapshotUsec_;
    }
    std::unique_lock<std::mutex> lock(a11ySnapshotMutex_);
    snapshotCv_.wait_for(
        lock, std::chrono::microseconds(timeoutUsec),
        [&] { return a11ySnapshotUsec_ != snap; });
}

void A11yMonitor::start() {
    if (running_.load()) return;
    stopRequested_.store(false);
    thread_ = std::thread(&A11yMonitor::threadFunc, this);
}

void A11yMonitor::stop() {
    stopRequested_.store(true);
    if (thread_.joinable())
        thread_.join();
}

void A11yMonitor::threadFunc() {
    running_.store(true);
    g_debugFlag = &debug_;

    DBusConnection *bus = connectAtspiBus();
    if (!bus) {
        running_.store(false);
        return;
    }

    // Register with AT-SPI2 registry for focus events
    DBusError err;
    dbus_error_init(&err);

    auto registerEvent = [&](const char *eventName) {
        DBusMessage *regMsg = dbus_message_new_method_call(
            "org.a11y.atspi.Registry", "/org/a11y/atspi/registry",
            "org.a11y.atspi.Registry", "RegisterEvent");
        if (regMsg) {
            dbus_message_append_args(regMsg, DBUS_TYPE_STRING, &eventName,
                                     DBUS_TYPE_INVALID);
            DBusMessage *reply = dbus_connection_send_with_reply_and_block(
                bus, regMsg, 2000, &err);
            if (reply) dbus_message_unref(reply);
            else {
                dbus_error_free(&err);
                dbus_error_init(&err);
            }
            dbus_message_unref(regMsg);
        }
    };

    registerEvent("object:state-changed:focused");
    registerEvent("focus:");

    dbus_bus_add_match(bus,
                       "type='signal',"
                       "interface='org.a11y.atspi.Event.Object',"
                       "member='StateChanged'",
                       &err);
    dbus_error_free(&err);
    dbus_error_init(&err);
    dbus_bus_add_match(bus,
                       "type='signal',"
                       "interface='org.a11y.atspi.Event.Focus'",
                       &err);
    dbus_error_free(&err);
    dbus_error_init(&err);
    // New connections joining the a11y bus (e.g. a browser starting up)
    dbus_bus_add_match(bus,
                       "type='signal',sender='org.freedesktop.DBus',"
                       "interface='org.freedesktop.DBus',"
                       "member='NameOwnerChanged'",
                       &err);
    dbus_error_free(&err);
    // Text/selection events on the focused entry trigger an immediate
    // snapshot re-poll (we don't parse the payloads — the signal only
    // tells us "something changed, re-query now").
    for (const char *member : {"TextChanged", "TextCaretMoved",
                               "TextSelectionChanged"}) {
        dbus_error_init(&err);
        std::string match = std::string(
                                "type='signal',"
                                "interface='org.a11y.atspi.Event.Object',"
                                "member='") +
                            member + "'";
        dbus_bus_add_match(bus, match.c_str(), &err);
        dbus_error_free(&err);
    }

    A11Y_LOG("A11yMonitor started");

    // Poke browsers already on the bus, then re-poke whenever a new app
    // connects (short + late retry: the app root only becomes queryable once
    // the browser's ATK bridge has registered with the registry), plus a
    // periodic sweep as a fallback.
    pokeA11yApps(bus);

    using Clock = std::chrono::steady_clock;
    const auto kNever = Clock::time_point::max();
    Clock::time_point pokeAt = kNever;
    Clock::time_point latePokeAt = kNever;
    Clock::time_point periodicPokeAt =
        Clock::now() + std::chrono::seconds(15);

    // Poll loop
    while (!stopRequested_.load()) {
        if (!dbus_connection_read_write(bus, 200))
            break;

        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(bus)) != nullptr) {
            const char *iface = dbus_message_get_interface(msg);
            const char *member = dbus_message_get_member(msg);

            bool isFocusEvent = false;

            if (iface && member &&
                strcmp(iface, "org.a11y.atspi.Event.Object") == 0 &&
                strcmp(member, "StateChanged") == 0) {
                DBusMessageIter iter;
                if (dbus_message_iter_init(msg, &iter) &&
                    dbus_message_iter_get_arg_type(&iter) ==
                        DBUS_TYPE_STRING) {
                    const char *stateName = nullptr;
                    dbus_message_iter_get_basic(&iter, &stateName);
                    if (stateName && strcmp(stateName, "focused") == 0) {
                        dbus_message_iter_next(&iter);
                        if (dbus_message_iter_get_arg_type(&iter) ==
                            DBUS_TYPE_INT32) {
                            dbus_int32_t d1 = 0;
                            dbus_message_iter_get_basic(&iter, &d1);
                            if (d1 == 1)
                                isFocusEvent = true;
                        }
                    }
                }
            }

            if (iface && member &&
                strcmp(iface, "org.a11y.atspi.Event.Focus") == 0)
                isFocusEvent = true;

            // Text/caret/selection changes on the tracked entry mark the
            // snapshot dirty → re-poll immediately (payloads unparsed).
            if (iface && member &&
                strcmp(iface, "org.a11y.atspi.Event.Object") == 0 &&
                (strcmp(member, "TextChanged") == 0 ||
                 strcmp(member, "TextCaretMoved") == 0 ||
                 strcmp(member, "TextSelectionChanged") == 0)) {
                const char *sigPath = dbus_message_get_path(msg);
                std::lock_guard<std::mutex> lock(focusEntryMutex_);
                if (sigPath && !focusEntryPath_.empty() &&
                    strcmp(sigPath, focusEntryPath_.c_str()) == 0) {
                    a11yPollDirty_ = true;
                }
            }

            if (iface && member &&
                strcmp(iface, "org.freedesktop.DBus") == 0 &&
                strcmp(member, "NameOwnerChanged") == 0) {
                const char *busName = nullptr;
                const char *oldOwner = nullptr;
                const char *newOwner = nullptr;
                DBusError nerr;
                dbus_error_init(&nerr);
                if (dbus_message_get_args(msg, &nerr,
                                          DBUS_TYPE_STRING, &busName,
                                          DBUS_TYPE_STRING, &oldOwner,
                                          DBUS_TYPE_STRING, &newOwner,
                                          DBUS_TYPE_INVALID) &&
                    newOwner && newOwner[0]) {
                    auto now = Clock::now();
                    pokeAt = now + std::chrono::milliseconds(600);
                    latePokeAt = now + std::chrono::milliseconds(3000);
                }
                dbus_error_free(&nerr);
            }

            if (isFocusEvent) {
                const char *sender = dbus_message_get_sender(msg);
                const char *path = dbus_message_get_path(msg);
                if (sender && path) {
                    int role = queryRole(bus, sender, path);
                    bool hasDocWeb = hasDocumentWebAncestor(
                        bus, sender, path);
                    // Only query the app directly for web content.
                    // Browser-UI elements (the Chromium omnibox) can
                    // stall AT-SPI calls, and on X11 the autosuggest
                    // polling shares this monitor thread — a stuck reply
                    // here delays the polling snapshot and breaks autofill
                    // detection.  For everything else resolve the pid
                    // through the a11y registry on the session bus (no app
                    // round-trip, cannot stall) so the engine gets a
                    // verified pid instead of falling back to full
                    // /proc scans.
                    int procId = hasDocWeb
                                     ? queryProcessId(bus, sender, path)
                                     : queryConnectionPid(sender);
                    focusProcessId_.store(procId,
                                          std::memory_order_relaxed);
                    bool isUI = !hasDocWeb;
                    // Track whether the focused element is a real text
                    // entry (role TEXT / ENTRY / DOCUMENT_TEXT).  A
                    // Chromium tab whose focus is NOT a text entry
                    // (clicking a Google Sheets cell focuses the
                    // document/combo box while caps still carry the
                    // previous editor's hints) cannot receive
                    // surrounding-text replacements — the engine routes
                    // those to Uinput.
                    textEntryFocused_.store(
                        role == 61 /*TEXT*/ || role == 79 /*ENTRY*/ ||
                            role == 94 /*DOCUMENT_TEXT*/,
                        std::memory_order_relaxed);
                    bool editable = false, multiline = false,
                         singleLine = false;
                    std::string states;
                    queryStates(bus, sender, path, editable, multiline,
                                singleLine, states);
                    browserUIFocused_.store(isUI,
                                           std::memory_order_relaxed);
                    passwordFocused_.store(role == ROLE_PASSWORD_TEXT,
                                          std::memory_order_relaxed);
                    // Snapshot for the engine: role + text-entry states +
                    // monotonic timestamp.  Taken AFTER the ancestor walk so
                    // the snapshot reflects the completed analysis.
                    focusRole_.store(role, std::memory_order_relaxed);
                    focusEditable_.store(editable,
                                         std::memory_order_relaxed);
                    focusMultiline_.store(multiline,
                                          std::memory_order_relaxed);
                    focusSingleLine_.store(singleLine,
                                           std::memory_order_relaxed);
                    focusInWebDoc_.store(hasDocWeb,
                                         std::memory_order_relaxed);
                    focusSnapshotUsec_.store(
                        static_cast<uint64_t>(
                            std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count() /
                            1000),
                        std::memory_order_relaxed);
                    A11Y_LOG("Focus: webDoc=%d role=%d(%s) editable=%d "
                             "multiline=%d singleLine=%d states=[%s] "
                             "pid=%d path=%s",
                             hasDocWeb, role, roleName(role), editable,
                             multiline, singleLine, states.c_str(), procId,
                             path);
                    // Track the focused text entry so the engine can
                    // query its content directly at replacement time.
                    static constexpr int ROLE_ENTRY = 79;
                    static constexpr int ROLE_TEXT = 61;
                    if (role == ROLE_ENTRY || role == ROLE_TEXT ||
                        role == ROLE_PASSWORD_TEXT) {
                        {
                            std::lock_guard<std::mutex> lock(
                                focusEntryMutex_);
                            focusEntryBus_ = sender;
                            focusEntryPath_ = path;
                        }
                        focusEntrySnapshotUsec_.store(
                            static_cast<uint64_t>(
                                std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count() /
                                1000),
                            std::memory_order_relaxed);
                    }
                }
            }

            dbus_message_unref(msg);
        }

        // Poll the focused text entry's content + selection so the
        // engine can compute exact BS counts without blocking the main
        // thread.  Throttled; failures leave the previous snapshot (the
        // engine treats stale snapshots as unavailable).
        {
            uint64_t nowUsec = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count() /
                1000);
            static constexpr uint64_t kSnapshotPollUsec = 150000;
            if (pollEnabled_.load(std::memory_order_relaxed) &&
                (a11yPollDirty_ ||
                 nowUsec - lastA11yPollUsec_ >= kSnapshotPollUsec)) {
                a11yPollDirty_ = false;
                lastA11yPollUsec_ = nowUsec;
                std::string busName, path;
                uint64_t snapUsec = 0;
                if (focusedTextEntry(busName, path, snapUsec)) {
                    std::string txt =
                        queryText(bus, busName.c_str(), path.c_str());
                    int selStart = -1, selEnd = -1;
                    if (querySelection(bus, busName.c_str(), path.c_str(),
                                       selStart, selEnd)) {
                        int oldSelStart, oldSelEnd;
                        std::string oldText;
                        {
                            std::lock_guard<std::mutex> lock(
                                a11ySnapshotMutex_);
                            oldSelStart = a11ySnapshotSelStart_;
                            oldSelEnd = a11ySnapshotSelEnd_;
                            oldText = a11ySnapshotText_;
                            a11ySnapshotText_ = txt;
                            a11ySnapshotSelStart_ = selStart;
                            a11ySnapshotSelEnd_ = selEnd;
                            a11ySnapshotUsec_ = nowUsec;
                        }
                        if (selStart != oldSelStart ||
                            selEnd != oldSelEnd || txt != oldText) {
                            A11Y_LOG("Snapshot: text='%s' sel=%d,%d",
                                     txt.c_str(), selStart, selEnd);
                        }
                    }
                }
            }
            // Wake any engine thread waiting on a fresh snapshot.
            snapshotCv_.notify_all();
        }

        auto now = Clock::now();
        if (now >= pokeAt || now >= latePokeAt || now >= periodicPokeAt) {
            if (now >= pokeAt) pokeAt = kNever;
            if (now >= latePokeAt) latePokeAt = kNever;
            if (now >= periodicPokeAt)
                periodicPokeAt = now + std::chrono::seconds(15);
            pokeA11yApps(bus);
        }
    }

    dbus_connection_unref(bus);
    running_.store(false);
    A11Y_LOG("A11yMonitor stopped");
}
