#include "a11y_monitor.h"
#include "sheets_cell_tracker.h"
#include "a11y_work_policy.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dbus/dbus.h>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// AT-SPI2 role constants (from atspi-constants.h)
static constexpr int ROLE_DOCUMENT_WEB = 95;
static constexpr int ROLE_DOCUMENT_FRAME = 82;
static constexpr int ROLE_PASSWORD_TEXT = 40;
// Chromium may emit a focus event for a nested accessibility node inside a
// contenteditable control. Facebook comments currently reach DOCUMENT_WEB at
// depth 22, so 20 incorrectly classifies that event as browser chrome.
static constexpr int MAX_ANCESTOR_DEPTH = 64;

static uint64_t monotonicUsec() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Text signals are useful only to the snapshot consumer. Keep the focus and
// Sheets selection listeners independent: native Wayland must still detect
// cells even though it never enables background text polling.
class TextEventSubscription {
public:
    void update(DBusConnection *bus, const std::string &sender,
                const std::string &path) {
        std::string target;
        if (!sender.empty() && !path.empty())
            target = ",sender='" + sender + "',path='" + path + "'";
        if (target == target_) return;
        for (const char *member : {"TextChanged", "TextCaretMoved",
                                   "TextSelectionChanged"}) {
            const std::string rule =
                std::string("type='signal',interface='org.a11y.atspi.Event.Object',member='") +
                member + "'";
            if (!target_.empty())
                dbus_bus_remove_match(bus, (rule + target_).c_str(), nullptr);
            if (!target.empty())
                dbus_bus_add_match(bus, (rule + target).c_str(), nullptr);
        }
        target_ = std::move(target);
    }
private:
    std::string target_;
};

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
        // Two launcher generations serve GetAddress on the session bus:
        //  - org.a11y.atspi.Bus /org/a11y/atspi/bus (older at-spi2-core)
        //  - org.a11y.Bus /org/a11y/bus with the org.a11y.Bus interface
        //    (current at-spi2-core — Mint 22.3 only serves this one, and
        //    asking the old name there fails, blinding the monitor,
        //    2026-09-15).
        struct {
            const char *name;
            const char *path;
            const char *iface;
        } const variants[] = {
            {"org.a11y.atspi.Bus", "/org/a11y/atspi/bus",
             "org.a11y.atspi.Bus"},
            {"org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus"},
        };
        for (const auto &v : variants) {
            dbus_error_free(&err);
            dbus_error_init(&err);
            DBusMessage *msg = dbus_message_new_method_call(
                v.name, v.path, v.iface, "GetAddress");
            if (!msg) continue;
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

static DBusConnection *connectAtspiBus(const std::string &addr) {
    DBusError err;
    dbus_error_init(&err);
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

// Per-element pid of the focused accessible (GetProcessId round-trip to
// the app).  Only queried for WEB content (browser-UI elements can stall
// AT-SPI and block the shared monitor thread).  On old Chrome this
// returned the per-tab RENDERER pid — the per-input identity visible in
// the Focus log; Chrome ≥150's native a11y answers -1 for it, which is
// why the engine no longer relies on it.  Kept as a log/analysis signal
// only (focusElementPid_), NOT consumed by the engine.
static int queryElementPid(DBusConnection *bus, const char *sender,
                           const char *path) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.a11y.atspi.Accessible", "GetProcessId");
    if (!msg) return -1;

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 200, &err);
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

// Resolve the pid of the focused app's AT-SPI connection via the D-Bus
// daemon (GetConnectionUnixProcessID on org.freedesktop.DBus) instead of
// round-tripping to the app.  GetProcessId on Chromium's a11y objects is
// unreliable: browser-UI elements can stall AT-SPI (blocking the shared
// monitor thread), and Chrome ≥150's native-a11y mode simply returns -1
// for it.  The daemon answers from its own bookkeeping and cannot stall.
// NOTE: this method lives on org.freedesktop.DBus (path
// /org/freedesktop/DBus) — org.a11y.Bus has NO such method.  It must be
// called on the SAME bus the monitor is connected to: accessibility runs
// on a DEDICATED at-spi bus (/run/user/<uid>/at-spi/bus_0), and the
// senders' unique names only resolve there — calling the session bus
// silently returns -1 for every focus event (both bugs fixed
// 2026-09-07).
static int queryConnectionPid(DBusConnection *bus, const char *sender) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixProcessID");
    if (!msg) {
        return -1;
    }
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &sender,
                             DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 500, &err);
    dbus_message_unref(msg);

    int pid = -1;
    if (reply && !dbus_error_is_set(&err)) {
        dbus_uint32_t p = 0; // reply signature is 'u'
        if (dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32, &p,
                                  DBUS_TYPE_INVALID))
            pid = static_cast<int>(p);
        dbus_message_unref(reply);
    }
    dbus_error_free(&err);
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
                             const char *path, int timeoutMs = 500,
                             bool *ok = nullptr) {
    if (ok) *ok = false;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.a11y.atspi.Text", "GetText");
    if (!msg) return {};
    dbus_int32_t start = 0, end = -1;
    dbus_message_append_args(msg, DBUS_TYPE_INT32, &start, DBUS_TYPE_INT32,
                             &end, DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, timeoutMs, &err);
    dbus_message_unref(msg);
    std::string text;
    if (reply && !dbus_error_is_set(&err)) {
        DBusMessageIter iter;
        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN &&
            dbus_message_iter_init(reply, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
            const char *s = nullptr;
            dbus_message_iter_get_basic(&iter, &s);
            if (s) {
                text = s;
                if (ok) *ok = true;
            }
        }
    }
    if (reply) dbus_message_unref(reply);
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
        // Standard AT-SPI replies contain two top-level int32s ("ii").
        // Retain compatibility with bridges wrapping them in a struct/variant.
        DBusMessageIter rit, var, st;
        DBusMessageIter *cur = &rit;
        bool parsed = false;
        if (dbus_message_iter_init(reply, &rit)) {
            if (dbus_message_iter_get_arg_type(cur) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(cur, &var);
                cur = &var;
            }
            if (dbus_message_iter_get_arg_type(cur) == DBUS_TYPE_STRUCT) {
                dbus_message_iter_recurse(cur, &st);
                cur = &st;
            }
            dbus_int32_t v1 = -1, v2 = -1;
            if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN &&
                dbus_message_iter_get_arg_type(cur) == DBUS_TYPE_INT32) {
                dbus_message_iter_get_basic(cur, &v1);
                if (dbus_message_iter_next(cur) &&
                    dbus_message_iter_get_arg_type(cur) == DBUS_TYPE_INT32) {
                    dbus_message_iter_get_basic(cur, &v2);
                    parsed = !dbus_message_iter_next(cur) &&
                             ((v1 == -1 && v2 == -1) ||
                              (v1 >= 0 && v2 >= v1));
                    if (parsed) {
                        selStart = v1;
                        selEnd = v2;
                    }
                }
            }
        }
        dbus_message_unref(reply);
        dbus_error_free(&err);
        return parsed;
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

// FB-specific ancestor-chain signatures (X11 routing: chat → Surr,
// comment/other inputs → Uinput).  The chat composer's chain carries
// FB_CHAT_ROLE_A immediately followed by FB_CHAT_ROLE_B (observed 3/3
// sessions); the comment box carries FB_COMMENT_ROLE (2/2).  Stable
// across tree rebuilds within a session.  Both extracted during the
// document-web ancestor walk.
static constexpr int FB_CHAT_ROLE_A = 87;
static constexpr int FB_CHAT_ROLE_B = 16;
static constexpr int FB_COMMENT_ROLE = 39;

static bool hasDocumentWebAncestor(DBusConnection *bus,
                                   const char *sender,
                                   const char *path,
                                   bool &chatSig, bool &commentSig,
                                   std::string &documentPath,
                                   std::string &framePath) {
    std::string curSender = sender;
    std::string curPath = path;
    chatSig = false;
    commentSig = false;
    int prevRole = -1;

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
        if (prevRole == FB_CHAT_ROLE_A && role == FB_CHAT_ROLE_B)
            chatSig = true;
        if (role == FB_COMMENT_ROLE)
            commentSig = true;
        prevRole = role;
        if (role == ROLE_DOCUMENT_WEB || role == ROLE_DOCUMENT_FRAME) {
            documentPath = parentPath;
            return true;
        }
        // Gecko (Firefox) does not expose the web document as an ancestor
        // of the Google Docs grid combo box (webDoc=0) — the WINDOW frame
        // (role 23) carries the tab title and serves as the title source.
        if (role == 23 /*FRAME*/ && framePath.empty())
            framePath = parentPath;

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
                             const char *path, int timeoutMs = 500) {
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
        bus, msg, timeoutMs, &err);
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

static std::string queryAccessibleId(DBusConnection *bus, const char *sender,
                                     const char *path) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        sender, path, "org.a11y.atspi.Accessible", "GetAttributes");
    if (!msg) return {};
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        bus, msg, 30, &err);
    dbus_message_unref(msg);
    std::string found;
    DBusMessageIter root, array;
    if (reply && dbus_message_iter_init(reply, &root) &&
        dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&root, &array);
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&array, &entry);
            const char *key = nullptr, *value = nullptr;
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&entry, &key);
                dbus_message_iter_next(&entry);
                if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
                    dbus_message_iter_get_basic(&entry, &value);
            }
            if (key && value && strcmp(key, "id") == 0) {
                found = value;
                break;
            }
            dbus_message_iter_next(&array);
        }
    }
    if (reply) dbus_message_unref(reply);
    dbus_error_free(&err);
    return found;
}

// Resolve only within the focused Sheets document, once per document. The
// name box updates on click; the reused editor's Name updates only after keys.
static std::string findSheetsNameBox(DBusConnection *bus, const char *sender,
                                     const std::string &documentPath) {
    std::deque<std::string> queue{documentPath};
    std::unordered_set<std::string> seen;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(250);
    while (!queue.empty() && seen.size() < 256 &&
           std::chrono::steady_clock::now() < deadline) {
        std::string path = std::move(queue.front());
        queue.pop_front();
        if (!seen.insert(path).second) continue;
        if (queryAccessibleId(bus, sender, path.c_str()) == "t-name-box")
            return path;
        DBusMessage *msg = dbus_message_new_method_call(
            sender, path.c_str(), "org.a11y.atspi.Accessible", "GetChildren");
        if (!msg) continue;
        DBusError err;
        dbus_error_init(&err);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(
            bus, msg, 30, &err);
        dbus_message_unref(msg);
        DBusMessageIter root, array;
        if (reply && dbus_message_iter_init(reply, &root) &&
            dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&root, &array);
            while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
                DBusMessageIter child;
                dbus_message_iter_recurse(&array, &child);
                const char *childBus = nullptr, *childPath = nullptr;
                if (dbus_message_iter_get_arg_type(&child) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&child, &childBus);
                    dbus_message_iter_next(&child);
                    if (dbus_message_iter_get_arg_type(&child) == DBUS_TYPE_OBJECT_PATH)
                        dbus_message_iter_get_basic(&child, &childPath);
                }
                if (childBus && childPath && strcmp(childBus, sender) == 0 &&
                    queue.size() < 512)
                    queue.emplace_back(childPath);
                dbus_message_iter_next(&array);
            }
        }
        if (reply) dbus_message_unref(reply);
        dbus_error_free(&err);
    }
    return {};
}

// NOTE (2026-09-15): the automatic org.a11y.Status.ScreenReaderEnabled
// fallback was REMOVED by deliberate user decision — silently flipping a
// session-wide flag is not transparent, and on GNOME/Cinnamon it syncs to
// gsettings and autostarts Orca (TTS while typing).  The manual remedy
// lives in the settings GUI (fcitx5-skey-settings, General tab): it writes
// --force-renderer-accessibility into ~/.config/<browser>-flags.conf with
// explicit user consent.  Chrome ≥150 on Wayland still wakes its tree from
// the GetRelationSet poke below where the stub exists (Ubuntu), and from a
// flag set by the desktop/other ATs where it does not.

static void pokeA11yApps(DBusConnection *bus, A11yPokeCache &cache) {
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
            if (!cache.due(appBus, appPath, monotonicUsec())) {
                dbus_message_iter_next(&arr);
                continue;
            }
            // Poke EVERY app, not just browser names: Electron apps
            // (antigravity-ide, VS Code forks) re-enable Chromium's
            // native accessibility on the same GetRelationSet/GetAttributes
            // trigger as Chrome, but their AT-SPI names don't match the
            // browser list — and after an fcitx5 restart their a11y tree
            // stays dead otherwise (no focus events for the integrated
            // terminal).  The query is read-only and cheap; non-Chromium
            // apps just return an empty relation set.
            std::string name;
            if (g_debugFlag && g_debugFlag->load(std::memory_order_relaxed))
                name = queryName(bus, appBus, appPath);
            (void)name; // logged below with the poke
            DBusMessage *poke = dbus_message_new_method_call(
                appBus, appPath, "org.a11y.atspi.Accessible",
                "GetRelationSet");
            if (poke) {
                DBusError perr;
                dbus_error_init(&perr);
                DBusMessage *preply =
                    dbus_connection_send_with_reply_and_block(
                        bus, poke, 500, &perr);
                cache.result(appBus, appPath,
                             preply && !dbus_error_is_set(&perr), monotonicUsec());
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

static bool isFocusGain(DBusMessage *msg) {
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) return false;
    const char *iface = dbus_message_get_interface(msg);
    if (iface && strcmp(iface, "org.a11y.atspi.Event.Focus") == 0) return true;
    if (!dbus_message_is_signal(msg, "org.a11y.atspi.Event.Object", "StateChanged"))
        return false;
    DBusMessageIter iter;
    const char *state = nullptr;
    dbus_int32_t focused = 0;
    if (!dbus_message_iter_init(msg, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) return false;
    dbus_message_iter_get_basic(&iter, &state);
    if (!state || strcmp(state, "focused") || !dbus_message_iter_next(&iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INT32) return false;
    dbus_message_iter_get_basic(&iter, &focused);
    return focused == 1;
}

A11yMonitor::A11yMonitor() {
    dbus_threads_init_default();
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

A11yMonitor::~A11yMonitor() {
    stop();
    if (wakeFd_ >= 0) close(wakeFd_);
    if (sheetsQueryBus_) {
        dbus_connection_close(sheetsQueryBus_);
        dbus_connection_unref(sheetsQueryBus_);
    }
}

bool A11yMonitor::currentSheetsCell(std::string &identity, std::string &cell) {
    std::string sender, path, nameBox, address;
    {
        std::lock_guard<std::mutex> lock(sheetsMutex_);
        sender = sheetsBus_;
        path = sheetsPath_;
        nameBox = sheetsNameBoxPath_;
        address = sheetsBusAddress_;
    }
    identity.clear();
    cell.clear();
    if (path.empty()) return false;
    identity = sender + path;
    if (address.empty() || nameBox.empty()) return true;
    if (sheetsQueryBus_ && !dbus_connection_get_is_connected(sheetsQueryBus_)) {
        dbus_connection_close(sheetsQueryBus_);
        dbus_connection_unref(sheetsQueryBus_);
        sheetsQueryBus_ = nullptr;
    }
    if (!sheetsQueryBus_) {
        DBusError err;
        dbus_error_init(&err);
        sheetsQueryBus_ = dbus_connection_open_private(address.c_str(), &err);
        if (sheetsQueryBus_) {
            dbus_connection_set_exit_on_disconnect(sheetsQueryBus_, false);
            if (!dbus_bus_register(sheetsQueryBus_, &err)) {
                dbus_connection_close(sheetsQueryBus_);
                dbus_connection_unref(sheetsQueryBus_);
                sheetsQueryBus_ = nullptr;
            }
        }
        dbus_error_free(&err);
    }
    if (sheetsQueryBus_)
        cell = queryText(sheetsQueryBus_, sender.c_str(), nameBox.c_str(), 30);
    return true;
}

std::string A11yMonitor::atspiBusAddress() { return getAtspiBusAddress(); }

bool A11yMonitor::focusedTextEntry(std::string &busName, std::string &path,
                                   uint64_t &snapshotUsec) const {
    return textCache_.focusedEntry(busName, path, snapshotUsec);
}

bool A11yMonitor::a11yState(std::string &text, int &selStart, int &selEnd,
                            uint64_t maxAgeUsec) const {
    return textCache_.read(text, selStart, selEnd, monotonicUsec(), maxAgeUsec);
}

void A11yMonitor::setPollingEnabled(bool enabled) {
    if (textCache_.setEnabled(enabled, monotonicUsec())) wakeMonitor();
}

void A11yMonitor::wakeMonitor() {
    // Coalesced, nonblocking notification. No DBus operation on the key thread.
    if (wakeFd_ >= 0) {
        uint64_t value = 1;
        while (write(wakeFd_, &value, sizeof(value)) < 0 && errno == EINTR) {}
    }
}

void A11yMonitor::waitForSnapshotUpdate(uint64_t timeoutUsec,
                                      uint64_t observed) const {
    textCache_.waitForUpdate(observed, timeoutUsec);
}

void A11yMonitor::start() {
    if (running_.load()) return;
    stopRequested_.store(false);
    thread_ = std::thread(&A11yMonitor::threadFunc, this);
}

void A11yMonitor::stop() {
    stopRequested_.store(true);
    wakeMonitor();
    if (thread_.joinable())
        thread_.join();
}

void A11yMonitor::threadFunc() {
    running_.store(true);
    g_debugFlag = &debug_;

    const std::string address = getAtspiBusAddress();
    {
        std::lock_guard<std::mutex> lock(sheetsMutex_);
        sheetsBusAddress_ = address;
    }
    DBusConnection *bus = connectAtspiBus(address);
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
    registerEvent("object:active-descendant-changed");
    registerEvent("object:selection-changed");
    registerEvent("object:property-change:accessible-name");

    dbus_bus_add_match(bus,
                       "type='signal',"
                       "interface='org.a11y.atspi.Event.Object',"
                       "member='StateChanged',arg0='focused'",
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
    // Sheets events remain subscribed even while text polling is disabled.
    // Text signals are subscribed to the current input on demand below.
    for (const char *member : {"ActiveDescendantChanged",
                               "SelectionChanged", "PropertyChange"}) {
        dbus_error_init(&err);
        std::string match = std::string(
                                "type='signal',"
                                "interface='org.a11y.atspi.Event.Object',"
                                "member='") +
                            member + "'";
        if (strcmp(member, "PropertyChange") == 0)
            match += ",arg0='accessible-name'";
        dbus_bus_add_match(bus, match.c_str(), &err);
        dbus_error_free(&err);
    }

    A11Y_LOG("A11yMonitor started");

    // skey is an assistive-technology client: the engine cannot track the
    // caret in Chromium-family browsers without their AT-SPI trees, and
    // Chrome ≥150 only builds its tree when it sees the system-wide
    // toolkit-accessibility flag at startup (verified 2026-09-15 on
    // CachyOS; the key lives in dconf, so it survives reboots — other
    // apps just expose their trees too, which is the flag's normal state
    // whenever any AT is in use).  Idempotent one-shot per fcitx5 start;
    // skipped when the user disabled AutoEnableA11y (they manage the
    // system accessibility state themselves).
    if (autoEnableA11y_.load(std::memory_order_relaxed)) {
        if (FILE *gs = popen("gsettings set org.gnome.desktop.interface "
                             "toolkit-accessibility true 2>/dev/null", "r")) {
            pclose(gs);
            A11Y_LOG("Ensured toolkit-accessibility=true");
        }
    } else {
        A11Y_LOG("AutoEnableA11y disabled — toolkit-accessibility untouched");
    }

    // Poke browsers already on the bus, then re-poke whenever a new app
    // connects (short + late retry: the app root only becomes queryable once
    // the browser's ATK bridge has registered with the registry), plus a
    // periodic sweep as a fallback.
    A11yPokeCache pokeCache;
    pokeA11yApps(bus, pokeCache);

    using Clock = std::chrono::steady_clock;
    const auto kNever = Clock::time_point::max();
    Clock::time_point pokeAt = kNever;
    Clock::time_point latePokeAt = kNever;
    Clock::time_point periodicPokeAt =
        Clock::now() + std::chrono::seconds(15);
    // Monitor-thread only. Match both sender and path: paths are not globally
    // unique, and selections in background tabs must never reset a word.
    std::string cellContainerBus, cellContainerPath;
    SheetsCellTracker sheetsCells;
    std::unordered_map<std::string, std::string> sheetsNameBoxes;
    TextEventSubscription textEvents;
    int busFd = -1;
    dbus_connection_get_unix_fd(bus, &busFd);

    // Poll loop
    while (!stopRequested_.load()) {
        int timeout = textCache_.waitMillis(monotonicUsec());
        if (wakeFd_ >= 0 && busFd >= 0) {
            // A sync query can have queued signals while waiting for its
            // reply. Drain them before waiting for more socket activity.
            if (dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS)
                timeout = 0;
            const short busEvents = POLLIN |
                (dbus_connection_get_outgoing_size(bus) > 0 ? POLLOUT : 0);
            pollfd fds[] = {{busFd, busEvents, 0}, {wakeFd_, POLLIN, 0}};
            if (poll(fds, 2, timeout) < 0 && errno != EINTR) break;
            if (fds[1].revents & POLLIN) {
                uint64_t value;
                while (read(wakeFd_, &value, sizeof(value)) < 0 && errno == EINTR) {}
            }
            if (!dbus_connection_read_write(bus, 0)) break;
        } else {
            // Preserve responsiveness if eventfd is unavailable (fd limit).
            if (!dbus_connection_read_write(bus, std::min(timeout, 10))) break;
        }
        if (stopRequested_.load()) break;

        // Coalesce only focus gains already queued in this batch. Keep all
        // text/selection/blur signals in order (Sheets depends on those).
        // A bounded drain prevents an event storm from starving timers/polls.
        std::vector<DBusMessage *> messages;
        size_t lastFocus = 0;
        while (messages.size() < 256) {
            auto *message = dbus_connection_pop_message(bus);
            if (!message) break;
            if (isFocusGain(message)) lastFocus = messages.size();
            messages.push_back(message);
        }
        for (size_t index = 0; index < messages.size(); ++index) {
            DBusMessage *msg = messages[index];
            if (index != lastFocus && isFocusGain(msg)) {
                dbus_message_unref(msg);
                continue;
            }
            const char *iface = dbus_message_get_interface(msg);
            const char *member = dbus_message_get_member(msg);

            bool isFocusEvent = false;

            // Verified on Chrome/Wayland: clicking another Sheets cell emits
            // PropertyChange("accessible-name", ..., variant "H27") on the
            // SAME editor, without focus or SelectionChanged events.
            if (iface && member &&
                strcmp(iface, "org.a11y.atspi.Event.Object") == 0 &&
                strcmp(member, "PropertyChange") == 0) {
                const char *sender = dbus_message_get_sender(msg);
                const char *path = dbus_message_get_path(msg);
                DBusMessageIter iter, value;
                if (sender && path && dbus_message_iter_init(msg, &iter) &&
                    dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
                    const char *property = nullptr;
                    dbus_message_iter_get_basic(&iter, &property);
                    if (property && strcmp(property, "accessible-name") == 0 &&
                        dbus_message_iter_next(&iter) &&
                        dbus_message_iter_next(&iter) &&
                        dbus_message_iter_next(&iter) &&
                        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
                        dbus_message_iter_recurse(&iter, &value);
                        if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
                            const char *name = nullptr;
                            dbus_message_iter_get_basic(&value, &name);
                            if (name && sheetsCells.update(sender, path, name)) {
                                // Diagnostic only. This event can lag behind
                                // the first keys in the new cell; the engine
                                // queries Name directly before each key.
                                A11Y_LOG("CellSelection: event=accessible-name cell=%s "
                                         "path=%s (observation only)", name, path);
                            }
                        }
                    }
                }
            }

            if (iface && member &&
                strcmp(iface, "org.a11y.atspi.Event.Object") == 0 &&
                (strcmp(member, "ActiveDescendantChanged") == 0 ||
                 strcmp(member, "SelectionChanged") == 0)) {
                const char *sender = dbus_message_get_sender(msg);
                const char *path = dbus_message_get_path(msg);
                if (sender && path && !cellContainerPath.empty() &&
                    cellContainerBus == sender && cellContainerPath == path) {
                    auto serial = cellSelectionSerial_.fetch_add(
                                      1, std::memory_order_release) + 1;
                    A11Y_LOG("CellSelection: event=%s serial=%llu path=%s",
                             member, static_cast<unsigned long long>(serial), path);
                }
            }

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
                            else
                                textCache_.blur(dbus_message_get_sender(msg),
                                                dbus_message_get_path(msg));
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
                textCache_.changed(dbus_message_get_sender(msg),
                                   dbus_message_get_path(msg));
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
                                          DBUS_TYPE_INVALID)) {
                    if (busName && oldOwner && oldOwner[0])
                        pokeCache.removeBus(busName);
                    if (newOwner && newOwner[0]) {
                        auto now = Clock::now();
                        pokeAt = now + std::chrono::milliseconds(600);
                        latePokeAt = now + std::chrono::milliseconds(3000);
                    }
                }
                dbus_error_free(&nerr);
            }

            if (isFocusEvent) {
                const char *sender = dbus_message_get_sender(msg);
                const char *path = dbus_message_get_path(msg);
                if (sender && path) {
                    // Invalidate before the potentially slow ancestor walk.
                    textCache_.focus({}, {}, monotonicUsec());
                    int role = queryRole(bus, sender, path);
                    bool fbChatSig = false, fbCommentSig = false;
                    std::string documentPath, framePath;
                    bool hasDocWeb = hasDocumentWebAncestor(
                        bus, sender, path, fbChatSig, fbCommentSig,
                        documentPath, framePath);
                    // Chrome ≥150 reports the cell editor as either the
                    // combo box (11) or the inner ENTRY (79, single-line) —
                    // states alone cannot separate it from real inputs.
                    // The accessible-id is the only reliable discriminator.
                    const bool sheetsEditor = hasDocWeb &&
                        (role == 11 || role == 79) &&
                        queryAccessibleId(bus, sender, path) == "waffle-rich-text-editor";
                    std::string nameBox;
                    if (sheetsEditor) {
                        const std::string key = std::string(sender) + documentPath;
                        auto cached = sheetsNameBoxes.find(key);
                        if (cached != sheetsNameBoxes.end()) nameBox = cached->second;
                        if (nameBox.empty()) {
                            nameBox = findSheetsNameBox(bus, sender, documentPath);
                            if (!nameBox.empty()) {
                                if (sheetsNameBoxes.size() >= 32) sheetsNameBoxes.clear();
                                sheetsNameBoxes[key] = nameBox;
                            }
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(sheetsMutex_);
                        sheetsBus_ = sheetsEditor ? sender : "";
                        sheetsPath_ = sheetsEditor ? path : "";
                        sheetsNameBoxPath_ = nameBox;
                    }
                    sheetsCells.focus(sender, path, sheetsEditor,
                                      sheetsEditor ? queryName(bus, sender, path) : "");
                    sheetsEditorFocused_.store(sheetsEditor,
                                               std::memory_order_release);
                    if (sheetsEditor)
                        A11Y_LOG("Sheets editor tracked: role=%d path=%s nameBox=%s",
                                 role, path, nameBox.c_str());
                    // Google Docs-suite page: the tab title is the document's
                    // Name (Chrome) or the window frame's Name (Gecko — the
                    // document is not an ancestor there, webDoc=0).  Firefox
                    // on these heavy canvas pages drops consecutive native
                    // deletes — the engine routes them to Uinput like
                    // Chrome's Sheets.
                    {
                        bool docsPage = false;
                        std::string titlePath = documentPath;
                        if (titlePath.empty())
                            titlePath = framePath;
                        if (!titlePath.empty()) {
                            std::string title =
                                queryName(bus, sender, titlePath.c_str());
                            std::string lower;
                            lower.reserve(title.size());
                            for (unsigned char c : title)
                                lower.push_back(static_cast<char>(std::tolower(c)));
                            if (lower.find("google") != std::string::npos) {
                                static const char *const kws[] = {
                                    "trang tính", "trang trình bày", "tài liệu",
                                    "biểu mẫu", "sheets", "docs", "slides",
                                    "forms", "jamboard",
                                };
                                for (const char *kw : kws) {
                                    if (lower.find(kw) != std::string::npos) {
                                        docsPage = true;
                                        break;
                                    }
                                }
                            }
                        }
                        googleDocsDocFocused_.store(docsPage,
                                                    std::memory_order_release);
                    }
                    // Sheets keeps focus on its combo box while the selected
                    // cell changes. Also accept a web table/tree-table, but
                    // never text editors or browser-UI autocomplete lists.
                    if (hasDocWeb && (role == 11 /*COMBO_BOX*/ ||
                                      role == 55 /*TABLE*/ ||
                                      role == 66 /*TREE_TABLE*/)) {
                        cellContainerBus = sender;
                        cellContainerPath = path;
                    } else {
                        cellContainerBus.clear();
                        cellContainerPath.clear();
                    }
                    focusFbChatSig_.store(fbChatSig,
                                          std::memory_order_relaxed);
                    focusFbCommentSig_.store(fbCommentSig,
                                             std::memory_order_relaxed);
                    // Resolve the pid through the D-Bus daemon for every
                    // focus event (web content included).  GetProcessId on
                    // Chromium's a11y objects stalls on browser-UI
                    // elements (blocking the shared monitor thread and the
                    // X11 autosuggest polling) and Chrome ≥150's native
                    // a11y answers -1 for web objects anyway — the
                    // connection-owner pid from the daemon is the browser
                    // process itself, which is exactly what the engine's
                    // /proc marker checks need.
                    int procId = queryConnectionPid(bus, sender);
                    focusProcessId_.store(procId,
                                          std::memory_order_relaxed);
                    // Per-element pid (renderer pid on old Chrome) —
                    // log/analysis signal only, not consumed by the
                    // engine.  Web content only: browser-UI GetProcessId
                    // can stall the shared monitor thread.
                    int elemPid = hasDocWeb && debug_.load(std::memory_order_relaxed)
                                      ? queryElementPid(bus, sender, path)
                                      : -1;
                    focusElementPid_.store(elemPid,
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
                             "pid=%d elemPid=%d path=%s",
                             hasDocWeb, role, roleName(role), editable,
                             multiline, singleLine, states.c_str(), procId,
                             elemPid, path);
                    // Only browser UI consumes text snapshots. Do not read
                    // passwords or web documents while the engine's focus
                    // verdict is still catching up with this event.
                    static constexpr int ROLE_ENTRY = 79;
                    static constexpr int ROLE_TEXT = 61;
                    if (!hasDocWeb && (role == ROLE_ENTRY || role == ROLE_TEXT)) {
                        textCache_.focus(sender, path, monotonicUsec());
                    }
                }
            }

            dbus_message_unref(msg);
        }

        {
            std::string sender, path;
            uint64_t stamp = 0;
            if (textCache_.active(monotonicUsec()))
                textCache_.focusedEntry(sender, path, stamp);
            textEvents.update(bus, sender, path);
        }

        // Bound event-driven polls, stop on idle, back off on errors, and
        // discard replies from a disabled polling generation. Sheets uses
        // its separate synchronous connection and is unaffected by this cache.
        if (auto request = textCache_.beginPoll(monotonicUsec())) {
            bool textOk = false;
            std::string txt = queryText(bus, request->bus.c_str(),
                                        request->path.c_str(), 500, &textOk);
            int selStart = -1, selEnd = -1;
            bool ok = textOk &&
                querySelection(bus, request->bus.c_str(), request->path.c_str(),
                               selStart, selEnd);
            if (debug_.load(std::memory_order_relaxed) && ok)
                A11Y_LOG("Snapshot: text='%s' sel=%d,%d",
                         txt.c_str(), selStart, selEnd);
            textCache_.finishPoll(*request, ok, std::move(txt),
                                  selStart, selEnd, monotonicUsec());
        }

        auto now = Clock::now();
        if (now >= pokeAt || now >= latePokeAt || now >= periodicPokeAt) {
            if (now >= pokeAt) pokeAt = kNever;
            if (now >= latePokeAt) latePokeAt = kNever;
            if (now >= periodicPokeAt)
                periodicPokeAt = now + std::chrono::seconds(15);
            pokeA11yApps(bus, pokeCache);
        }
    }

    textEvents.update(bus, {}, {});
    dbus_connection_unref(bus);
    running_.store(false);
    A11Y_LOG("A11yMonitor stopped");
}
