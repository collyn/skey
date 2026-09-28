// Run only under dbus-run-session. This fake AT-SPI application exercises the
// real monitor thread, subscriptions, wakeups and private Sheets connection.
// It never connects to the user's accessibility bus or changes system settings.
#include "a11y_monitor.h"
#include "sheets_cell_tracker.h"
#include <dbus/dbus.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>

static int checks = 0;
static void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
static bool until(const std::function<bool()> &predicate, int timeoutMs = 1500) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

class FakeAtspi {
public:
    FakeAtspi() {
        DBusError error;
        dbus_error_init(&error);
        bus = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
        check(bus != nullptr, "connect isolated test bus");
        dbus_connection_set_exit_on_disconnect(bus, false);
        name = dbus_bus_get_unique_name(bus);
        check(dbus_bus_request_name(bus, "org.a11y.atspi.Registry", 0, &error) ==
              DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER, "own fake registry");
        dbus_error_free(&error);
        worker = std::thread([this] { serve(); });
    }
    ~FakeAtspi() {
        stop = true;
        worker.join();
        dbus_connection_close(bus);
        dbus_connection_unref(bus);
    }
    void focus(const char *path) {
        auto *signal = dbus_message_new_signal(path, "org.a11y.atspi.Event.Focus", "Focus");
        send(signal);
    }
    void textChanged(const char *path) {
        auto *signal = dbus_message_new_signal(path, "org.a11y.atspi.Event.Object", "TextChanged");
        send(signal);
    }
    void lostFocus(const char *path) {
        auto *signal = dbus_message_new_signal(path, "org.a11y.atspi.Event.Object", "StateChanged");
        const char *state = "focused";
        dbus_int32_t focused = 0;
        dbus_message_append_args(signal, DBUS_TYPE_STRING, &state,
                                 DBUS_TYPE_INT32, &focused, DBUS_TYPE_INVALID);
        send(signal);
    }
    void delayedCellName(const char *cell) {
        auto *signal = dbus_message_new_signal("/sheet", "org.a11y.atspi.Event.Object", "PropertyChange");
        const char *prop = "accessible-name";
        dbus_int32_t zero = 0;
        dbus_message_append_args(signal, DBUS_TYPE_STRING, &prop,
                                 DBUS_TYPE_INT32, &zero, DBUS_TYPE_INT32, &zero, DBUS_TYPE_INVALID);
        DBusMessageIter root, variant;
        dbus_message_iter_init_append(signal, &root);
        dbus_message_iter_open_container(&root, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &cell);
        dbus_message_iter_close_container(&root, &variant);
        send(signal);
    }
    std::string name;
    std::atomic<int> textReads{0}, cellReads{0}, cell{24};
    std::atomic<bool> failCell{false}, failText{false};
    std::atomic<int> roleReads{0};
    std::atomic<bool> holdRole{false}, roleWaiting{false};
private:
    void send(DBusMessage *message) {
        dbus_connection_send(bus, message, nullptr);
        dbus_connection_flush(bus);
        dbus_message_unref(message);
    }
    void serve() {
        while (!stop) {
            dbus_connection_read_write(bus, 10);
            while (auto *message = dbus_connection_pop_message(bus)) {
                if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
                    dbus_message_unref(message);
                    continue;
                }
                const char *method = dbus_message_get_member(message);
                const char *path = dbus_message_get_path(message);
                auto *reply = dbus_message_new_method_return(message);
                if (!std::strcmp(method, "GetRole")) {
                    ++roleReads;
                    if (holdRole) {
                        roleWaiting = true;
                        while (holdRole && !stop)
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        roleWaiting = false;
                    }
                    dbus_uint32_t role = !std::strcmp(path, "/doc") ? 95 :
                                        !std::strcmp(path, "/button") ? 43 : 79;
                    dbus_message_append_args(reply, DBUS_TYPE_UINT32, &role, DBUS_TYPE_INVALID);
                } else if (!std::strcmp(method, "Get")) {
                    const char *iface = nullptr, *prop = nullptr;
                    dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &iface,
                                          DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
                    DBusMessageIter root, variant, value;
                    dbus_message_iter_init_append(reply, &root);
                    if (prop && !std::strcmp(prop, "Parent")) {
                        dbus_message_iter_open_container(&root, DBUS_TYPE_VARIANT, "(so)", &variant);
                        dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, nullptr, &value);
                        const char *sender = name.c_str();
                        const char *parent = !std::strcmp(path, "/sheet") ? "/doc" : "/org/a11y/atspi/null";
                        dbus_message_iter_append_basic(&value, DBUS_TYPE_STRING, &sender);
                        dbus_message_iter_append_basic(&value, DBUS_TYPE_OBJECT_PATH, &parent);
                        dbus_message_iter_close_container(&variant, &value);
                    } else {
                        dbus_message_iter_open_container(&root, DBUS_TYPE_VARIANT, "s", &variant);
                        const char *title = !std::strcmp(path, "/doc") ? "Test - Google Sheets" : "F24";
                        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &title);
                    }
                    dbus_message_iter_close_container(&root, &variant);
                } else if (!std::strcmp(method, "GetAttributes")) {
                    DBusMessageIter root, array, entry;
                    dbus_message_iter_init_append(reply, &root);
                    dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{ss}", &array);
                    const char *id = !std::strcmp(path, "/sheet") ? "waffle-rich-text-editor" :
                                     !std::strcmp(path, "/namebox") ? "t-name-box" : nullptr;
                    if (id) {
                        const char *key = "id";
                        dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
                        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
                        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &id);
                        dbus_message_iter_close_container(&array, &entry);
                    }
                    dbus_message_iter_close_container(&root, &array);
                } else if (!std::strcmp(method, "GetChildren")) {
                    DBusMessageIter root, array, entry;
                    dbus_message_iter_init_append(reply, &root);
                    dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "(so)", &array);
                    if (!std::strcmp(path, "/doc")) {
                        const char *sender = name.c_str(), *child = "/namebox";
                        dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, nullptr, &entry);
                        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &sender);
                        dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH, &child);
                        dbus_message_iter_close_container(&array, &entry);
                    }
                    dbus_message_iter_close_container(&root, &array);
                } else if (!std::strcmp(method, "GetState")) {
                    DBusMessageIter root, array;
                    dbus_message_iter_init_append(reply, &root);
                    dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "u", &array);
                    dbus_uint32_t states = (1u << 6) | (1u << 25), zero = 0;
                    dbus_message_iter_append_basic(&array, DBUS_TYPE_UINT32, &states);
                    dbus_message_iter_append_basic(&array, DBUS_TYPE_UINT32, &zero);
                    dbus_message_iter_close_container(&root, &array);
                } else if (!std::strcmp(method, "GetText")) {
                    const bool namebox = !std::strcmp(path, "/namebox");
                    if (namebox) ++cellReads; else ++textReads;
                    if ((namebox && failCell) || (!namebox && failText)) {
                        dbus_message_unref(reply);
                        reply = dbus_message_new_error(message, DBUS_ERROR_FAILED, "test failure");
                    } else {
                        std::string content = namebox ? "F" + std::to_string(cell.load()) : "gõ example";
                        const char *text = content.c_str();
                        dbus_message_append_args(reply, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
                    }
                } else if (!std::strcmp(method, "GetSelection")) {
                    dbus_int32_t start = 3, end = 10;
                    dbus_message_append_args(reply, DBUS_TYPE_INT32, &start,
                                             DBUS_TYPE_INT32, &end, DBUS_TYPE_INVALID);
                }
                send(reply);
                dbus_message_unref(message);
            }
        }
    }
    DBusConnection *bus = nullptr;
    std::atomic<bool> stop{false};
    std::thread worker;
};

int main() {
    const char *address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    check(address && *address, "test requires dbus-run-session");
    setenv("AT_SPI_BUS_ADDRESS", address, 1);
    dbus_threads_init_default();
    FakeAtspi app;
    A11yMonitor monitor;
    monitor.setAutoEnableA11y(false);
    monitor.start();
    std::string bus, path, text;
    uint64_t stamp = 0;
    int start, end;
    check(until([&] {
        app.focus("/entry");
        return monitor.focusedTextEntry(bus, path, stamp);
    }), "monitor must receive focus on isolated bus");
    // Let the final duplicate focus event drain before testing snapshots.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(app.textReads == 0, "disabled/native-Wayland mode must not read text");
    monitor.setPollingEnabled(true);
    check(until([&] { return monitor.a11yState(text, start, end, 400000); }), "demand must wake monitor");
    check(text == "gõ example" && start == 3 && end == 10, "real IPC snapshot must retain text/selection");
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    const int idleReads = app.textReads;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    check(app.textReads == idleReads, "idle input must stop GetText calls");
    monitor.setPollingEnabled(true);
    check(until([&] { return app.textReads > idleReads; }), "first key after idle must resume without a text event");
    app.failText = true;
    app.textChanged("/entry");
    check(until([&] { return !monitor.a11yState(text, start, end, 400000); }), "failed query must invalidate snapshot");
    app.failText = false;
    monitor.setPollingEnabled(true);
    check(until([&] { return monitor.a11yState(text, start, end, 400000); }), "polling must recover after failure");
    app.lostFocus("/entry");
    check(until([&] { return !monitor.focusedTextEntry(bus, path, stamp); }), "focus loss must clear input identity");

    // Sheets remains independently tracked with background text polling off.
    monitor.setPollingEnabled(false);
    // Queue a burst while one query is in flight. Only the newest queued
    // focus should trigger another expensive role/ancestor analysis.
    const int rolesBefore = app.roleReads;
    app.holdRole = true;
    app.focus("/button");
    check(until([&] { return app.roleWaiting.load(); }), "hold focus query for burst test");
    for (int i = 0; i < 50; ++i) app.focus(i % 2 ? "/entry" : "/button");
    app.holdRole = false;
    check(until([&] { return monitor.focusedTextEntry(bus, path, stamp) && path == "/entry"; }),
          "latest focus in burst must win");
    check(app.roleReads - rolesBefore <= 3, "queued focus burst must not walk all 50 ancestors");
    app.focus("/sheet");
    check(until([&] { return monitor.sheetsEditorFocused(); }), "recognize actual Sheets editor via attributes");
    SheetsCellSnapshot cells;
    std::string identity, cell;
    check(monitor.currentSheetsCell(identity, cell) && cell == "F24", "read actual name box with polling disabled");
    check(!cells.observe(identity, cell), "initial cell must only seed baseline");
    app.cell = 25;
    check(monitor.currentSheetsCell(identity, cell) && cell == "F25" && cells.observe(identity, cell),
          "new cell must reset before first key without waiting for events");
    app.delayedCellName("F24");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(monitor.currentSheetsCell(identity, cell) && cell == "F25" && !cells.observe(identity, cell),
          "late old-cell event must not overwrite synchronous baseline");
    app.failCell = true;
    check(monitor.currentSheetsCell(identity, cell) && cell.empty() && !cells.observe(identity, cell),
          "failed Sheets read must preserve baseline");
    app.failCell = false;
    app.cell = 26;
    check(monitor.currentSheetsCell(identity, cell) && cell == "F26" && cells.observe(identity, cell),
          "next Sheets read must retry immediately and detect new cell");
    app.cell = 27;
    check(monitor.currentSheetsCell(identity, cell) && cell == "F27" && cells.observe(identity, cell),
          "pending commit boundary must detect another cell change");
    check(monitor.currentSheetsCell(identity, cell) && !cells.observe(identity, cell),
          "first key after commit boundary must not reset twice");
    app.focus("/button");
    check(until([&] { return !monitor.sheetsEditorFocused(); }), "leaving Sheets must clear recognition");
    check(!monitor.currentSheetsCell(identity, cell), "normal UI must not query old Sheets name box");
    monitor.stop();
    check(!monitor.isRunning(), "monitor must stop cleanly");
    std::cout << "A11y integration: " << checks << " checks passed\n";
}
