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
                fprintf(f, "[a11y] " fmt "\n", ##__VA_ARGS__);                  \
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

static bool isChromiumBrowserAppName(const std::string &name) {
    // Keep in sync with isChromiumBrowser() in engine.cpp; these are the
    // application names exposed on the AT-SPI bus ("Google Chrome", ...).
    static constexpr const char *kBrowserNames[] = {
        "chrome", "chromium", "brave", "vivaldi", "edge", "opera",
    };
    std::string lower;
    lower.reserve(name.size());
    for (char c : name)
        lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (const char *b : kBrowserNames)
        if (lower.find(b) != std::string::npos)
            return true;
    return false;
}

static void pokeChromiumBrowsers(DBusConnection *bus) {
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
            std::string name = queryName(bus, appBus, appPath);
            if (isChromiumBrowserAppName(name)) {
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

    A11Y_LOG("A11yMonitor started");

    // Poke browsers already on the bus, then re-poke whenever a new app
    // connects (short + late retry: the app root only becomes queryable once
    // the browser's ATK bridge has registered with the registry), plus a
    // periodic sweep as a fallback.
    pokeChromiumBrowsers(bus);

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
                    bool hasDocWeb =
                        hasDocumentWebAncestor(bus, sender, path);
                    bool isUI = !hasDocWeb;
                    browserUIFocused_.store(isUI,
                                           std::memory_order_relaxed);
                    A11Y_LOG("Focus: browserUI=%d path=%s", isUI, path);
                }
            }

            dbus_message_unref(msg);
        }

        auto now = Clock::now();
        if (now >= pokeAt || now >= latePokeAt || now >= periodicPokeAt) {
            if (now >= pokeAt) pokeAt = kNever;
            if (now >= latePokeAt) latePokeAt = kNever;
            if (now >= periodicPokeAt)
                periodicPokeAt = now + std::chrono::seconds(15);
            pokeChromiumBrowsers(bus);
        }
    }

    dbus_connection_unref(bus);
    running_.store(false);
    A11Y_LOG("A11yMonitor stopped");
}
