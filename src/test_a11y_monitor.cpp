// Exercise the production DBus parsers with real DBusMessage payloads. Only
// transport is substituted: no live desktop, browser or accessibility settings
// are touched, and delayed/error replies are deterministic.
#include <dbus/dbus.h>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <string>
#include <vector>

static std::vector<std::string> addedMatches, removedMatches;
static void testAddMatch(DBusConnection *, const char *rule, DBusError *) {
    addedMatches.emplace_back(rule);
}
static void testRemoveMatch(DBusConnection *, const char *rule, DBusError *) {
    removedMatches.emplace_back(rule);
}

static DBusMessage *nextReply = nullptr;
static bool timeoutReply = false;
static int lastTimeout = 0;
static int textStart = -2, textEnd = -2;
static DBusMessage *testReply(DBusConnection *, DBusMessage *request,
                              int timeout, DBusError *error) {
    lastTimeout = timeout;
    if (dbus_message_is_method_call(request, "org.a11y.atspi.Text", "GetText")) {
        dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32, &textStart,
                             DBUS_TYPE_INT32, &textEnd, DBUS_TYPE_INVALID);
    }
    if (timeoutReply) {
        dbus_set_error_const(error, DBUS_ERROR_NO_REPLY, "test timeout");
        return nullptr;
    }
    auto *reply = nextReply;
    nextReply = nullptr;
    return reply;
}
#define dbus_connection_send_with_reply_and_block testReply
#define dbus_bus_add_match testAddMatch
#define dbus_bus_remove_match testRemoveMatch
#include "a11y_monitor.cpp"
#undef dbus_connection_send_with_reply_and_block
#undef dbus_bus_add_match
#undef dbus_bus_remove_match

static int checks = 0;
static void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

static DBusMessage *selectionReply(int start, int end, int wrapper = 0) {
    auto *reply = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
    DBusMessageIter root, variant, structure;
    dbus_message_iter_init_append(reply, &root);
    auto *iter = &root;
    if (wrapper == 2) {
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "(ii)", &variant);
        iter = &variant;
    }
    if (wrapper) {
        dbus_message_iter_open_container(iter, DBUS_TYPE_STRUCT, nullptr, &structure);
        iter = &structure;
    }
    dbus_int32_t a = start, b = end;
    dbus_message_iter_append_basic(iter, DBUS_TYPE_INT32, &a);
    dbus_message_iter_append_basic(iter, DBUS_TYPE_INT32, &b);
    if (wrapper)
        dbus_message_iter_close_container(wrapper == 2 ? &variant : &root, &structure);
    if (wrapper == 2)
        dbus_message_iter_close_container(&root, &variant);
    return reply;
}

static DBusMessage *stringReply(const char *value) {
    auto *reply = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
    return reply;
}

static void testParsers() {
    int a, b;
    for (int wrapper = 0; wrapper != 3; ++wrapper) {
        nextReply = selectionReply(2, 7, wrapper);
        check(querySelection(nullptr, "org.test.App", "/entry", a, b) && a == 2 && b == 7,
              "standard/struct/variant selection must retain offsets");
    }
    nextReply = selectionReply(4, 4);
    check(querySelection(nullptr, "org.test.App", "/entry", a, b) && a == 4 && b == 4,
          "collapsed selection must not become unavailable");
    nextReply = selectionReply(-1, -1);
    check(querySelection(nullptr, "org.test.App", "/entry", a, b) && a == -1 && b == -1,
          "no selection must remain distinct from caret zero");
    for (auto range : {std::pair<int, int>{7, 2}, {-1, 2}, {2, -1}}) {
        nextReply = selectionReply(range.first, range.second);
        check(!querySelection(nullptr, "org.test.App", "/entry", a, b) && a == -1 && b == -1,
              "invalid range must fail without leaking offsets");
    }
    nextReply = stringReply("bad payload");
    check(!querySelection(nullptr, "org.test.App", "/entry", a, b),
          "wrong selection signature must fail");
    nextReply = selectionReply(2, 7);
    dbus_int32_t extra = 8;
    dbus_message_append_args(nextReply, DBUS_TYPE_INT32, &extra, DBUS_TYPE_INVALID);
    check(!querySelection(nullptr, "org.test.App", "/entry", a, b),
          "extra selection argument must fail");
    nextReply = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
    dbus_message_append_args(nextReply, DBUS_TYPE_INT32, &extra, DBUS_TYPE_INVALID);
    check(!querySelection(nullptr, "org.test.App", "/entry", a, b),
          "incomplete selection must fail");

    bool ok = false;
    nextReply = stringReply("");
    check(queryText(nullptr, "org.test.App", "/entry", 500, &ok).empty() && ok,
          "valid empty text is a successful snapshot");
    nextReply = stringReply("gõ tiếng Việt");
    check(queryText(nullptr, "org.test.App", "/entry", 500, &ok) == "gõ tiếng Việt" && ok,
          "text snapshot preserves UTF-8");
    nextReply = selectionReply(2, 7);
    check(queryText(nullptr, "org.test.App", "/entry", 500, &ok).empty() && !ok,
          "wrong text signature must not look like a valid empty input");
    timeoutReply = true;
    check(queryText(nullptr, "org.test.App", "/entry", 500, &ok).empty() && !ok,
          "text timeout must not publish empty success");
    check(!querySelection(nullptr, "org.test.App", "/entry", a, b),
          "selection timeout must fail");
    timeoutReply = false;

    // Sheets still performs an uncached read of the name box with its original
    // 30ms timeout and complete range, including immediately after a failure.
    for (const char *cell : {"F24", "G24", "" , "H24"}) {
        nextReply = stringReply(cell);
        check(queryText(nullptr, "org.test.Sheets", "/namebox", 30) == cell &&
              lastTimeout == 30 && textStart == 0 && textEnd == -1,
              "Sheets cell reads must not be truncated/cached by text polling");
    }
}

static void testFocusAndPublication() {
    A11yTextCache cache;
    std::string text;
    int a, b;
    check(!cache.beginPoll(1000), "no poll without demand");
    cache.setEnabled(true, 1000);
    check(!cache.beginPoll(1000), "no poll without an input");
    cache.focus(":1.1", "/entry", 1000);
    auto request = cache.beginPoll(1000);
    check(request.has_value(), "focus plus demand must poll");
    check(cache.finishPoll(*request, true, "abc", 1, 3, 1100), "publish current focus");
    check(cache.read(text, a, b, 1200, 400000) && text == "abc" && a == 1 && b == 3,
          "read published text and selection");
    cache.focus(":1.2", "/entry", 1300);
    check(!cache.read(text, a, b, 1300, 400000) && text.empty() && a == -1 && b == -1,
          "same path in another app must invalidate snapshot and output arguments");
    check(!cache.finishPoll(*request, true, "old app", 0, 7, 1400),
          "late old-focus reply must be rejected");
    request = cache.beginPoll(1400);
    check(request.has_value(), "new focus must poll immediately");
    cache.setEnabled(false, 1500);
    cache.setEnabled(true, 1600);
    check(!cache.finishPoll(*request, true, "before disable", 0, 1, 1700),
          "disable/re-enable must invalidate an in-flight request");
    request = cache.beginPoll(1700);
    cache.finishPoll(*request, true, "current", -1, -1, 1800);
    cache.blur(":1.other", "/entry");
    check(cache.read(text, a, b, 1900, 400000), "other app blur must not clear focus");
    cache.blur(":1.2", "/entry");
    check(!cache.beginPoll(2000) && !cache.read(text, a, b, 2000, 400000),
          "blur must clear both identity and snapshot");
    cache.focus(":1.2", "/entry", 2100);
    request = cache.beginPoll(2100);
    cache.focus({}, {}, 2200);
    check(!cache.finishPoll(*request, true, "old input", -1, -1, 2300),
          "focus on a non-input must reject an outstanding reply");
}

static void testScheduling() {
    A11yTextCache cache;
    cache.focus(":1.1", "/entry", 1000);
    check(cache.setEnabled(true, 1000), "first demand must wake idle monitor");
    check(cache.waitMillis(1000) == 0, "first demand must poll immediately");
    check(!cache.setEnabled(true, 1000), "active keystrokes must not signal extra wakeups");
    auto request = cache.beginPoll(1000);
    cache.finishPoll(*request, true, "text", -1, -1, 1100);
    cache.changed(":1.other", "/entry");
    check(!cache.beginPoll(20000), "same path from another sender must not trigger polling");
    cache.changed(":1.1", "/other");
    check(!cache.beginPoll(20000), "background input must not trigger polling");
    cache.changed(":1.1", "/entry");
    check(!cache.beginPoll(14999), "dirty events must respect minimum interval");
    request = cache.beginPoll(16000);
    check(request.has_value(), "coalesced dirty event must eventually poll");
    cache.finishPoll(*request, true, "new", -1, -1, 17000);
    check(!cache.beginPoll(165999), "unchanged input must use periodic interval");
    check(cache.beginPoll(166000).has_value(), "periodic fallback must remain available");
    check(!cache.active(501000) && !cache.beginPoll(501000), "idle must stop text IPC");
    cache.setEnabled(true, 600000);
    request = cache.beginPoll(600000);
    check(request.has_value(), "typing after idle must resume immediately");
    check(cache.finishPoll(*request, true, "resumed", -1, -1, 600100), "resumed poll publishes");
    std::string text;
    int a, b;
    check(!cache.read(text, a, b, 1000001, 400000), "stale snapshots must be unavailable");

    cache.changed(":1.1", "/entry");
    request = cache.beginPoll(620000);
    check(request.has_value(), "poll before simulated failure");
    cache.finishPoll(*request, false, "", -1, -1, 620100);
    check(!cache.read(text, a, b, 620200, 400000), "failure must invalidate previous success");
    cache.changed(":1.1", "/entry");
    cache.setEnabled(true, 700000);
    check(!cache.beginPoll(700000), "typing/dirty events must not defeat failure backoff");
    request = cache.beginPoll(770100);
    check(request.has_value(), "failed input must be retried after backoff");
    cache.finishPoll(*request, true, "recovered", -1, -1, 770200);
    check(cache.read(text, a, b, 770300, 400000) && text == "recovered",
          "successful retry must restore snapshots");

    // A reply that took 450ms must not masquerade as a just-captured snapshot.
    cache.setEnabled(true, 800000);
    cache.changed(":1.1", "/entry");
    request = cache.beginPoll(800000);
    cache.setEnabled(true, 1200000);
    cache.finishPoll(*request, true, "slow reply", -1, -1, 1250000);
    check(!cache.read(text, a, b, 1250000, 400000), "freshness must date from request start");
}

static void testEventStormAndWait() {
    A11yTextCache cache;
    cache.focus(":1.1", "/entry", 1000);
    cache.setEnabled(true, 1000);
    int polls = 0;
    for (uint64_t time = 1000; time < 501000; time += 1000) {
        cache.changed(":1.1", "/entry");
        if (auto request = cache.beginPoll(time)) {
            ++polls;
            cache.finishPoll(*request, true, "abc", -1, -1, time);
        }
    }
    check(polls == 34, "500 dirty events must coalesce into 34 polls, not 500");
    check(!cache.beginPoll(501000), "event storm alone must not extend activity lease");

    cache.setEnabled(true, 600000);
    const auto observed = cache.stamp();
    auto request = cache.beginPoll(600000);
    cache.finishPoll(*request, true, "arrived before wait", -1, -1, 600100);
    const auto before = std::chrono::steady_clock::now();
    cache.waitForUpdate(observed, 1000000);
    check(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(200),
          "update between read and wait must not cause a lost wakeup");

    // Exercise the same concurrent writers/readers as engine/monitor threads.
    std::thread writer([&] {
        for (uint64_t time = 700000; time < 800000; time += 1000) {
            cache.setEnabled(true, time);
            cache.changed(":1.1", "/entry");
            if (auto poll = cache.beginPoll(time))
                cache.finishPoll(*poll, true, "parallel", 1, 2, time);
        }
    });
    for (int i = 0; i < 1000; ++i) {
        std::string text;
        int a, b;
        cache.read(text, a, b, 800000, 400000);
        cache.stamp();
    }
    writer.join();
}

static void testSubscriptions() {
    TextEventSubscription subscription;
    subscription.update(nullptr, {}, {});
    check(addedMatches.empty(), "Wayland/idle must not subscribe to text signals");
    subscription.update(nullptr, ":1.1", "/entry");
    check(addedMatches.size() == 3, "active input subscribes to three text events");
    for (const auto &rule : addedMatches) {
        check(rule.find("sender=':1.1',path='/entry'") != std::string::npos,
              "text events must be scoped to both sender and path");
        check(rule.find("member='Text") != std::string::npos,
              "text subscription must not alter Sheets cell/focus listeners");
    }
    subscription.update(nullptr, ":1.1", "/entry");
    check(addedMatches.size() == 3 && removedMatches.empty(),
          "repeated keys must reuse subscriptions without DBus traffic");
    subscription.update(nullptr, ":1.2", "/entry");
    check(addedMatches.size() == 6 && removedMatches.size() == 3,
          "same path in a different app must replace subscriptions");
    subscription.update(nullptr, {}, {});
    check(removedMatches == addedMatches, "idle/focus loss must remove all text subscriptions");
}

int main() {
    testParsers();
    testFocusAndPublication();
    testScheduling();
    testEventStormAndWait();
    testSubscriptions();
    std::cout << "A11y monitor: " << checks << " checks passed\n";
}
