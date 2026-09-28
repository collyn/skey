#include "engine.h"
#include "input_timing.h"
#include "a11y_work_policy.h"
#include "x11_app_name.h"
#include <fcitx-utils/event.h>
#include <fcitx/focusgroup.h>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>

static void check(bool value, const char *message);

// A small application model, not a LibreOffice/Wayland compositor emulator.
// Delay IM commits so a raw letter can overtake them in the Wayland case.
// The assertion is on visible UTF-8 text, independently of viet_'s state.
class OfficeTextInput : public fcitx::InputContext {
public:
    OfficeTextInput(fcitx::InputContextManager &manager, bool wayland)
        : InputContext(manager, "soffice.bin"), wayland_(wayland) { created(); }
    ~OfficeTextInput() override { destroy(); }
    const char *frontend() const override { return "test"; }
    std::string text;
    void flushCommits() { text += pending_; pending_.clear(); }
    void key(const fcitx::Key &key, bool release) {
        if (release) return;
        if (key.check(FcitxKey_BackSpace)) {
            flushCommits();
            check(!text.empty(), "injected deletion must not cross the start of the word");
            size_t last = text.size() - 1;
            while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xc0) == 0x80) --last;
            text.erase(last);
        } else {
            if (!wayland_) flushCommits();
            text += fcitx::Key::keySymToUTF8(key.sym());
            // Adversarial ordering: physical append lands before the IM
            // commit, just as a queued a could overtake the committed đ.
            flushCommits();
        }
    }
protected:
    void commitStringImpl(const std::string &value) override { pending_ += value; }
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &event) override {
        key(event.rawKey(), event.isRelease());
    }
    void deleteSurroundingTextImpl(int, unsigned) override {
        check(false, "Uinput regression must not use native surrounding deletion");
    }
    void updatePreeditImpl() override {}
private:
    bool wayland_;
    std::string pending_;
};

static int checks = 0;
static void check(bool value, const char *message) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

class TestInput : public fcitx::InputContext {
public:
    explicit TestInput(fcitx::InputContextManager &manager, const std::string &program = "skey-test")
        : InputContext(manager, program) { created(); }
    ~TestInput() override { destroy(); }
    const char *frontend() const override { return "test"; }
    std::vector<std::string> commits;
    struct Forward { fcitx::Key key; bool release; int time; };
    std::vector<Forward> forwarded;
    int deletions = 0;
protected:
    void commitStringImpl(const std::string &text) override { commits.push_back(text); }
    void deleteSurroundingTextImpl(int, unsigned size) override { deletions += size; }
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &event) override {
        forwarded.push_back({event.rawKey(), event.isRelease(), event.time()});
    }
    void updatePreeditImpl() override {}
};

namespace fcitx {
struct EnginePerformanceTest {
    static void preeditVisibility(Instance &instance, bool wayland, bool client) {
        SKeyEngine engine(&instance, false);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager());
        input.setFocusGroup(&group);
        input.setCapabilityFlags(client ? CapabilityFlags(CapabilityFlag::Preedit) : CapabilityFlags());
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "skey-test";
        state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::Preedit;
        engine.config_.showPreedit.setValue(true);
        auto press = [&](KeySym sym) {
            KeyEvent event(&input, Key(sym));
            state.keyEvent(event);
            check(event.accepted(), "preedit typing must consume the key");
        };
        auto visible = [&](const std::string &expected) {
            check(input.inputPanel().clientPreedit().toString() == (client ? expected : "") &&
                      input.inputPanel().preedit().toString() == (client ? "" : expected),
                  "ShowPreedit must control inline and fallback-panel visibility");
        };
        press(FcitxKey_g);
        press(FcitxKey_o);
        visible("go");
        engine.config_.showPreedit.setValue(false);
        state.refreshPreeditVisibility();
        visible("");
        check(state.viet_.getComposed() == "go" && input.commits.empty(),
              "hiding preedit must neither reset nor commit the composition");
        press(FcitxKey_x);
        visible("");
        check(state.viet_.getComposed() == "gõ", "hidden composition must still process tone keys");
        engine.config_.showPreedit.setValue(true);
        state.refreshPreeditVisibility();
        visible("gõ");
        // A capability change must not leave the old destination visible.
        client = !client;
        input.setCapabilityFlags(client ? CapabilityFlags(CapabilityFlag::Preedit) : CapabilityFlags());
        state.refreshPreeditVisibility();
        visible("gõ");
        engine.config_.showPreedit.setValue(false);
        state.refreshPreeditVisibility();
        KeyEvent space(&input, Key(FcitxKey_space));
        state.keyEvent(space);
        visible("");
        std::string committed;
        for (const auto &text : input.commits) committed += text;
        check(committed == "gõ" && !space.accepted() && state.viet_.getRawInput().empty(),
              "space must commit hidden preedit exactly once");

        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.modeCacheValid_ = true;
        state.viet_.setRawInput("gõ");
        engine.config_.showPreedit.setValue(true);
        state.refreshPreeditVisibility();
        visible("");
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.refreshPreeditVisibility();
        visible("");
        state.cachedMode_ = SKeyOutputMode::Preedit;
        state.modeMenuActive_ = true;
        state.refreshPreeditVisibility();
        visible("");
        state.modeMenuActive_ = false;
        engine.config_.outputMode.setValue(SKeyOutputMode::Preedit);
        engine.config_.showPreedit.setValue(false);
        state.deactivate();
        check(!state.preeditWasPending_ && engine.pendingPreedits_["skey-test"] == "gõ",
              "hidden preedit must be saved without assuming the app auto-committed it");
        const auto commitsBeforeFocus = input.commits.size();
        state.activate();
        check(input.commits.size() == commitsBeforeFocus + 1 && input.commits.back() == "gõ" &&
                  engine.pendingPreedits_.empty(),
              "returning to the same input must recover hidden preedit exactly once");
    }

    static void firefoxNativeReplacement(Instance &instance) {
        class FirefoxInput : public TestInput {
        public:
            explicit FirefoxInput(InputContextManager &manager) : TestInput(manager, "firefox") {}
            std::string text;
            unsigned cursor = 0;
        protected:
            void commitStringImpl(const std::string &value) override {
                commits.push_back(value);
                text += value;
                ++cursor; // this scenario commits one code point at a time
                surroundingText().setText(text, cursor, cursor);
                updateSurroundingText();
            }
            void deleteSurroundingTextImpl(int offset, unsigned size) override {
                check(offset == -1 && size == 1 && text == "go",
                      "Firefox native request must delete only the old o");
                ++deletions;
                text.pop_back();
                --cursor;
                // Engine mirrors the delete; publish the next snapshot on commit.
            }
        };
        SKeyEngine engine(&instance, false);
        FocusGroup group("x11:test", instance.inputContextManager());
        FirefoxInput input(instance.inputContextManager());
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText); // Url absent, as in log
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "firefox";
        state.cachedIsChromium_ = state.cachedIsTerminalApp_ = 0;
        state.cachedIsFirefoxOrSnap_ = 1;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        // A private transport ensures a regression cannot inject real keys.
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "create Firefox regression transport");
        state.uinputClientFd_ = sockets[0];
        for (char ch : std::string("gox")) {
            KeyEvent event(&input, Key(static_cast<KeySym>(ch)));
            state.keyEvent(event);
            check(event.accepted(), "Firefox surrounding mode consumes each Telex key");
            state.reset(); // Firefox resets after each commit in the report
        }
        check(input.text == "gõ" && input.deletions == 1 && input.forwarded.empty(),
              "Firefox X11 go+x must replace o through native deletion, not append goõ");
        check(!state.uinputDeleting_ && !state.hasDeferredCommitPending() &&
                  state.viet_.getComposed() == "gõ",
              "Firefox reset must preserve completed native replacement");
        char request[16];
        check(recv(sockets[1], request, sizeof(request), MSG_DONTWAIT) < 0 &&
                  errno == EAGAIN,
              "matching Firefox surrounding text must not send Uinput Backspace");
        close(sockets[0]);
        state.uinputClientFd_ = -1;
        close(sockets[1]);
    }

    static void firefoxFallback(Instance &instance, int snapshot, bool docs) {
        SKeyEngine engine(&instance, false);
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
        engine.a11yMonitor_->googleDocsDocFocused_ = docs;
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "firefox");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "firefox";
        state.cachedIsChromium_ = state.cachedIsTerminalApp_ = 0;
        state.cachedIsFirefoxOrSnap_ = 1;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        if (snapshot != 0) {
            input.surroundingText().setText(snapshot == 1 ? "go" : "zz", 2, 2);
            input.updateSurroundingText();
        }
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "create Firefox fallback transport");
        state.uinputClientFd_ = sockets[0];
        state.surroundingCommit("go", "gõ");
        uint32_t request[4] = {};
        check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request) && request[0] == 2,
              "Firefox missing/stale surrounding or Docs must retain Uinput fallback");
        check(input.deletions == 0 && input.commits.empty() && state.uinputDeleting_,
              "fallback must wait for Backspace before committing");
        state.resetForCellChange();
        close(sockets[0]);
        state.uinputClientFd_ = -1;
        close(sockets[1]);
    }

    static void officeWord(Instance &instance, bool wayland,
                           const std::string &keys, int arrival) {
        std::cout << "Office word: " << (wayland ? "Wayland" : "X11")
                  << " keys=" << keys << " arrival=" << arrival << std::endl;
        SKeyEngine engine(&instance, false);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        OfficeTextInput input(instance.inputContextManager(), wayland);
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "soffice.bin";
        state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        // Capture actual server requests without connecting to the desktop
        // uinput service or injecting any keys into the user's applications.
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "create isolated uinput transport");
        state.uinputClientFd_ = sockets[0]; // owned/closed by SKeyState
        auto press = [&](char ch) {
            KeyEvent event(&input, Key(static_cast<KeySym>(ch)));
            state.keyEvent(event);
            if (!event.accepted()) input.key(event.rawKey(), false);
        };
        int replacements = 0;
        int realDeletes = 0;
        bool burstSent = false;
        auto burst = [&] {
            burstSent = true;
            for (size_t i = 2; i < keys.size(); ++i) press(keys[i]);
        };
        auto drain = [&] {
            while (state.uinputDeleting_) {
                check(++replacements <= 4, "replacement replay must terminate");
                uint32_t request[4] = {};
                const auto received = recv(sockets[1], request, sizeof(request), 0);
                if (received != sizeof(request))
                    std::cerr << "uinput recv=" << received << " errno=" << errno
                              << " fd=" << state.uinputClientFd_ << " expected="
                              << state.expectedUinputBackspaces_ << "\n";
                check(received == sizeof(request),
                      "replacement must send one complete uinput v3 request");
                check(request[0] >= 2 && request[0] <= 3 && request[1] == 0 && request[2] == 0,
                      "request contains suffix deletions plus one sync anchor");
                for (uint32_t i = 0; i < request[0]; ++i) {
                    KeyEvent event(&input, Key(FcitxKey_BackSpace));
                    state.keyEvent(event);
                    if (i + 1 == request[0]) {
                        check(event.accepted(), "sync anchor must never delete application text");
                    } else {
                        check(!event.accepted(), "real backspace must reach application");
                        input.key(event.rawKey(), false);
                        ++realDeletes;
                    }
                }
                check(state.uinputSettling_, "anchor must schedule the commit");
                if (arrival == 2 && !burstSent) burst();
                // Advance the settle boundary deterministically. Existing run()
                // tests exercise the real event-loop timer and cancellation.
                state.finishUinputSettle(0, 0);
            }
        };
        press('d');
        press('d');
        if (arrival == 1) burst(); // all remaining keys before the sync anchor
        drain();
        if (arrival == 0) {
            for (size_t i = 2; i < keys.size(); ++i) {
                press(keys[i]);
                drain();
            }
        }
        input.flushCommits();
        if (input.text != "đây") std::cerr << "Actual application text: " << input.text << '\n';
        check(input.text == "đây", "application text must retain đ for ddaay and ddaya");
        check(replacements == 2 && realDeletes == (keys == "ddaay" ? 2 : 3),
              "one- and two-character suffix replacements delete exactly the intended text");
        check(state.viet_.getComposed() == "đây" && state.committedLen_ == 3 &&
                  state.bufferedUinputKeys_.empty() && state.settledKeys_.empty() &&
                  !state.uinputSettling_ && state.uinputBsOutstanding_ == 0,
              "composition and transaction queues must agree with application text");
        close(sockets[1]);
    }

    static void officeAppendOrdering(Instance &instance, bool wayland,
                                     const char *program, bool expectCommit) {
        SKeyEngine engine(&instance, false);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), program);
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::Uinput;

        KeyEvent first(&input, Key(FcitxKey_d));
        state.keyEvent(first);
        check(first.accepted() == expectCommit,
              "office Wayland letters must use commits; other apps retain raw keys");
        input.commits.clear();

        // dd has deleted d and is waiting to commit đ. A physical a arrives
        // while settling, as in the LibreOffice report. Replaying it through
        // forwardKey would allow the app's raw-key queue to overtake đ.
        state.viet_.setRawInput("đ");
        state.uinputDeleting_ = true;
        state.pendingUinputCommit_ = "đ";
        state.uinputPendingFinalLen_ = 1;
        state.scheduleUinputSettle(50000, 0, 0);
        KeyEvent append(&input, Key(FcitxKey_a));
        state.keyEvent(append);
        check(append.accepted() && input.commits.empty(), "append must wait for pending replacement");
        state.finishUinputSettle(0, 0);
        check(!input.commits.empty() && input.commits[0] == "đ", "replacement commits first");
        if (expectCommit) {
            check(input.commits.size() == 2 && input.commits[1] == "a" && input.forwarded.empty(),
                  "queued office append must follow replacement on the same commit channel");
        } else {
            check(input.commits.size() == 1 && input.forwarded.size() == 1 &&
                      input.forwarded[0].key.sym() == FcitxKey_a,
                  "other apps retain forwarded append during replay");
        }
        check(state.viet_.getComposed() == "đa" && state.committedLen_ == 2,
              "append routing must preserve composition and length");
    }

    static void electronBackspace(Instance &instance) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "antigravity-ide");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "antigravity-ide";
        state.cachedIsChromium_ = 1;
        state.cachedIsTerminalApp_ = state.cachedIsFirefoxOrSnap_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.viet_.setRawInput("bạn");
        state.committedLen_ = 3;
        // Terminal helper textarea publishes a valid but stale snapshot and
        // ignores native deletes. TestInput deliberately does not acknowledge
        // deletes/forwarded keys by publishing a new surrounding snapshot.
        input.surroundingText().setText("các bạn", 7, 7);
        input.updateSurroundingText();
        for (int i = 0; i < 12; ++i) {
            KeyEvent backspace(&input, Key(FcitxKey_BackSpace), false, 1000 + i * 40);
            state.keyEvent(backspace);
            check(backspace.accepted() && input.forwarded.size() == static_cast<size_t>(i + 1),
                  "each Electron user Backspace must reach app even with a stale valid snapshot");
            check(input.deletions == 0 && !state.hasDeferredCommitPending() && !state.uinputDeleting_,
                  "held Electron Backspace must not rely on native deletes or wait for repair");
        }
        check(state.viet_.getRawInput().empty() && !state.reclaimReady_,
              "deleting across word boundary must not retain an erased word for reclaim");
        check(input.surroundingText().cursor() == 7,
              "forwarded Electron Backspace must not fabricate successful surrounding deletion");
        state.viet_.setRawInput("bạn");
        state.saveLastWord();
        state.viet_.reset();
        state.wordWasBackspaced_ = false;
        input.surroundingText().setText("bạn ", 4, 4);
        input.updateSurroundingText();
        KeyEvent separator(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(separator);
        check(state.reclaimReady_ && state.sepAlreadyDeleted_,
              "first Electron Backspace after space preserves tone-edit reclaim");
        KeyEvent previousWord(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(previousWord);
        check(!state.reclaimReady_ && input.deletions == 0,
              "continuing deletion clears reclaim without native delete");
        input.surroundingText().setText("selected", 8, 0);
        input.updateSurroundingText();
        const auto before = input.forwarded.size();
        KeyEvent selection(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(selection);
        check(input.forwarded.size() == before + 1 && input.deletions == 0 && !state.reclaimReady_,
              "Electron selection deletion is handled once by the application");
        state.viet_.setRawInput("bạn");
        state.committedLen_ = 3;
        KeyEvent composingSelection(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(composingSelection);
        check(input.forwarded.size() == before + 2 && state.viet_.getRawInput().empty() &&
              state.committedLen_ == 0 && input.deletions == 0,
              "selection deletion must clear active composition rather than retain one-char undo");
    }

    static void nativeBackspaceUnchanged(Instance &instance, bool wayland, const char *program, bool chromium) {
        SKeyEngine engine(&instance, false);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), program);
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        state.cachedIsChromium_ = chromium;
        state.cachedIsTerminalApp_ = state.cachedIsFirefoxOrSnap_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.viet_.setRawInput("bạn");
        state.committedLen_ = 3;
        input.surroundingText().setText("bạn", 3, 3);
        input.updateSurroundingText();
        KeyEvent backspace(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(backspace);
        check(input.deletions == 1 && input.forwarded.empty(),
              "browser, non-Electron and X11 native deletion policy must remain unchanged");
    }

    static void addressBarIdentity(Instance &instance, bool wayland) {
        SKeyEngine engine(&instance, false);
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
        auto &monitor = *engine.a11yMonitor_;
        // Simulate a recent Chrome UI snapshot left behind on app switch.
        monitor.browserUIFocused_ = true;
        monitor.focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        for (const char *name : {"electron", "code", "code-insiders", "antigravity-ide",
                                 "Tabby", "Hyper", "Slack", "Discord", "chrome-notes",
                                 "chromium-editor", "cooperation", "brave-editor",
                                 "com.example.chrome", "chrome-app-abcdef", "firefox", "zen"}) {
            TestInput input(instance.inputContextManager(), name);
            input.setFocusGroup(&group);
            auto &state = *input.propertyFor(&engine.factory_);
            // Worst case: runtime was identified as Chromium and geometry,
            // Url hint and a previous browser verdict all resemble an omnibox.
            state.cachedIsChromium_ = 1;
            input.setCursorRect(Rect(10, 20, 11, 40));
            input.setCapabilityFlags(CapabilityFlags(CapabilityFlag::SurroundingText) | CapabilityFlag::Url);
            state.addrBarUiVerdictAtUsec_ = now(CLOCK_MONOTONIC);
            check(!state.inChromiumAddressBar(), "Electron/non-browser Url must not select omnibox routing");
            check(state.addrBarUiVerdictAtUsec_ == 0, "non-browser must clear old omnibox latch");
            input.setCapabilityFlags(CapabilityFlag::SurroundingText);
            state.addrBarUiVerdictAtUsec_ = now(CLOCK_MONOTONIC);
            check(!state.inChromiumAddressBar(), "Electron caret/latch must not select omnibox routing");
            engine.config_.chromiumAddressBarMode.setValue(SKeyChromiumAddressBarMode::Preedit);
            engine.config_.outputMode.setValue(SKeyOutputMode::Uinput);
            state.cachedProgram_ = name; // isolate from user's per-app configuration
            state.modeCacheValid_ = false;
            check(state.effectiveMode() == SKeyOutputMode::Uinput,
                  "address-bar configuration must not override Electron mode");
        }
        for (const char *name : {"google-chrome-stable", "Chromium", "Brave-browser",
                                 "vivaldi-snapshot", "microsoft-edge-dev", "opera",
                                 "com.google.Chrome.desktop", "org.chromium.Chromium",
                                 "/opt/google/chrome/chrome"}) {
            TestInput input(instance.inputContextManager(), name);
            input.setFocusGroup(&group);
            auto &state = *input.propertyFor(&engine.factory_);
            input.setCapabilityFlags(CapabilityFlags(CapabilityFlag::SurroundingText) | CapabilityFlag::Url);
            check(state.inChromiumAddressBar(), "known browser Url retains omnibox routing");
            input.setCapabilityFlags(CapabilityFlag::SurroundingText);
            input.setCursorRect(Rect());
            check(!state.inChromiumAddressBar(), "browser web input without Url/caret must not be omnibox");
            if (wayland) {
                input.setCursorRect(Rect(10, 20, 11, 40));
                check(!state.inChromiumAddressBar(), "Wayland find bar must not use X11 caret heuristic");
            } else {
                const double scale = std::clamp(x11DisplayDpi() / 96.0, 1.0, 2.5);
                Rect caret;
                caret.setPosition(10, 20).setSize(1, static_cast<int>(20 * scale));
                input.setCursorRect(caret);
                check(state.inChromiumAddressBar(), "X11 browser retains a11y/caret omnibox detection");
                monitor.focusInWebDoc_ = true;
                monitor.focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
                check(!state.inChromiumAddressBar() && state.addrBarUiVerdictAtUsec_ == 0,
                      "fresh Sheets/web focus must override old X11 omnibox latch and thin caret");
                monitor.focusInWebDoc_ = false;
            }
        }
        if (wayland) {
            TestInput input(instance.inputContextManager(), "");
            input.setFocusGroup(&group);
            auto &state = *input.propertyFor(&engine.factory_);
            state.appNameAttempted_ = true;
            state.resolvedProgram_ = "google-chrome";
            input.setCapabilityFlags(CapabilityFlag::Url);
            check(state.appProgram().empty() && !state.inChromiumAddressBar(),
                  "unknown native Wayland app must not inherit cached X11 Chrome identity");
        }
    }

    static void cancelPending(Instance &instance) {
        SKeyEngine engine(&instance, false);
        TestInput input(instance.inputContextManager());
        auto &state = *input.propertyFor(&engine.factory_);
        state.uinputDeleting_ = true;
        state.pendingUinputCommit_ = "previous cell";
        state.scheduleUinputSettle(50000, 0, 0);
        KeyEvent queued(&input, Key(FcitxKey_x));
        state.keyEvent(queued);
        auto cancel = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1000, 1,
            [&](EventSourceTime *, uint64_t) { state.resetForCellChange(); return true; });
        auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 80000, 1,
            [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
        instance.eventLoop().exec();
        check(input.commits.empty() && input.forwarded.empty() && !state.uinputSettling_,
              "cancelled cell timer must never commit or replay into new cell");
    }

    static void boundedRepair(Instance &instance) {
        SKeyEngine engine(&instance, false);
        TestInput input(instance.inputContextManager());
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        input.surroundingText().setText("ab", 2, 2);
        input.updateSurroundingText();
        state.deferredCommitText_ = "á";
        state.deferredDeletedTail_ = "b";
        state.deferredNativeDeleteLen_ = 1;
        state.finishDeferredCommit(0);
        auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 100000, 1,
            [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
        instance.eventLoop().exec();
        check(input.deletions == 2 && input.commits.size() == 1 && !state.hasDeferredCommitPending(),
              "unresponsive native delete must have bounded retries and one commit");
    }

    static void run(Instance &instance, bool wayland) {
        SKeyEngine engine(&instance, false);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager());
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        engine.appDelayOverrides_[skey::appDelayKey("skey-test", wayland)].preCommitMs = 80;
        state.uinputDeleting_ = true;
        state.expectedUinputBackspaces_ = state.seenUinputBackspaces_ = 1;
        state.pendingUinputCommit_ = "gõ";
        state.uinputPendingFinalLen_ = 2;
        state.bsSentAt_ = now(CLOCK_MONOTONIC) - 2000;
        Key control(FcitxKey_Left, KeyState::Ctrl);
        // A preceding replay may leave keys queued while a new cycle waits
        // for its backspace anchor. The anchor must bypass that queue.
        state.settledKeys_.push_back({control, false, 1234});
        KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(anchor);
        check(anchor.accepted() && state.expectedUinputBackspaces_ == 0,
              "anchor must bypass keys queued behind the next replacement");
        check(state.uinputSettling_ && input.commits.empty(), "anchor must schedule, not block/commit inline");
        KeyEvent release(&input, control, true, 1235);
        state.keyEvent(release);
        check(release.accepted() && input.forwarded.empty(), "keys must wait behind commit");
        bool heartbeat = false;
        auto beat = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1000, 1,
            [&](EventSourceTime *, uint64_t) { heartbeat = input.commits.empty(); return true; });
        auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 120000, 1,
            [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
        instance.eventLoop().exec();
        check(heartbeat, "event loop must run during 80ms settle");
        if (input.commits.size() != 1)
            std::cerr << "settle result: commits=" << input.commits.size()
                      << " settling=" << state.uinputSettling_
                      << " pending=" << state.pendingUinputCommit_ << '\n';
        check(input.commits.size() == 1 && input.commits[0] == "gõ", "commit exactly once");
        check(input.forwarded.size() == 2 && input.forwarded[0].key == control &&
              !input.forwarded[0].release && input.forwarded[0].time == 1234 &&
              input.forwarded[1].release && input.forwarded[1].time == 1235,
              "Ctrl key/release/timestamps must survive queued replay");
    }

    static void runNative(Instance &instance, bool wayland) {
        SKeyEngine engine(&instance, false);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager());
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);

        // Cached byte offset must not survive a same-size, same-cursor text update.
        input.surroundingText().setText("éab", 2, 2);
        input.updateSurroundingText();
        check(state.surroundingCacheEndsWith(input.surroundingText(), "a"), "initial UTF-8 offset");
        input.surroundingText().setText("abé", 2, 2);
        input.updateSurroundingText();
        check(state.surroundingCacheEndsWith(input.surroundingText(), "b"), "same-size update invalidates cursor cache");
        state.mirrorSurroundingDelete(-1, 1);
        check(state.surroundingCacheEndsWith(input.surroundingText(), "a"), "local mirrored deletion invalidates cursor cache");

        // Let an app's delayed surrounding push arrive between repair passes.
        input.commits.clear();
        input.surroundingText().setText("ab", 2, 2);
        input.updateSurroundingText();
        state.deferredCommitText_ = "á";
        state.deferredDeletedTail_ = "b";
        state.deferredNativeDeleteLen_ = 1;
        state.nativeRepairAttempt_ = 0;
        state.finishDeferredCommit(0);
        check(input.deletions == 0 && input.commits.empty(), "first missing-delete observation only schedules retry");
        KeyEvent navigation(&input, Key(FcitxKey_End), false, 2000);
        state.keyEvent(navigation);
        check(navigation.accepted() && input.forwarded.empty(), "navigation must wait for native repair commit");
        auto update = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1000, 1,
            [&](EventSourceTime *, uint64_t) {
                input.surroundingText().setText("a", 1, 1);
                input.updateSurroundingText();
                return true;
            });
        auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 45000, 1,
            [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
        instance.eventLoop().exec();
        if (input.deletions != 0 || input.commits.size() != 1)
            std::cerr << "repair result: deletes=" << input.deletions
                      << " commits=" << input.commits.size()
                      << " attempts=" << state.nativeRepairAttempt_
                      << " pending=" << state.deferredCommitText_ << '\n';
        check(input.deletions == 0 && input.commits.size() == 1 && input.commits[0] == "á",
              "fresh surrounding push must prevent duplicate native deletion");
        check(input.forwarded.size() == 1 && input.forwarded[0].key.sym() == FcitxKey_End,
              "navigation must replay after native repair");

        // Cell-change reset must cancel both kinds of pending commit and keys.
        state.uinputDeleting_ = state.uinputSettling_ = true;
        state.pendingUinputCommit_ = "old cell";
        state.settledKeys_.push_back({Key(FcitxKey_x), false, 5});
        state.deferredCommitText_ = "old deferred cell";
        state.resetForCellChange();
        state.finishUinputSettle(0, 0);
        state.finishDeferredCommit(0);
        check(input.commits.size() == 1 && state.settledKeys_.empty(), "cell switch must discard pending transactions");
    }
};
}

static void policies() {
    SurroundingCursor cursor;
    std::string longText;
    longText.reserve(1500000);
    for (int i = 0; i < 500000; ++i) longText += "ữ";
    for (int i = 0; i < 10000; ++i)
        check(cursor.endsWith(longText, 500000, "ữ"), "large UTF-8 input reuses correct cursor index");
    check(cursor.endsWith(longText, 100, "ữ"), "moving cursor invalidates cached position");
    check(!cursor.endsWith(longText, 0, "ữ"), "cursor at start has no suffix");
    check(skey::deferredDelay(0, 3000) == 15000, "unseeded default delay");
    check(skey::deferredDelay(3000, 3000) == 15000, "seed default delay");
    check(skey::deferredDelay(50000, 3000) == 100000, "adaptive delay capped at 100ms");
    check(skey::deferredDelay(std::numeric_limits<uint64_t>::max(), 3000) == 100000, "delay saturation cannot overflow");
    check(skey::updatedRoundTrip(100000, 10000, 3000, .3) == 55000, "latency recovers after spike");
    check(skey::updatedRoundTrip(0, 9000000, 3000, .3) == 200000, "stall sample cannot poison estimate");
    A11yPokeCache cache;
    check(cache.due(":1.1", "/root", 0), "new app needs activation");
    cache.result(":1.1", "/root", true, 0);
    check(!cache.due(":1.1", "/root", 600000) && cache.due(":1.1", "/root", 3000000), "keep delayed startup retry");
    cache.result(":1.1", "/root", true, 3000000);
    check(!cache.due(":1.1", "/root", 15000000) && cache.due(":1.1", "/root", 63000000), "healthy app cooldown and recovery refresh");
    cache.removeBus(":1.1");
    check(cache.due(":1.1", "/root", 15000001), "restart invalidates activation cache");
    cache.result(":1.1", "/root", false, 0);
    cache.result(":1.1", "/root", false, 1000000);
    check(!cache.due(":1.1", "/root", 2000000) && cache.due(":1.1", "/root", 3000000), "failed activations back off");
    check(cache.due(":1.2", "/root", 0), "cache must include sender");
}

int main(int argc, char **argv) {
    policies();
    {
        fcitx::Instance instance(argc, argv);
        for (bool wayland : {false, true})
            for (bool client : {false, true})
                fcitx::EnginePerformanceTest::preeditVisibility(instance, wayland, client);
        fcitx::EnginePerformanceTest::firefoxNativeReplacement(instance);
        fcitx::EnginePerformanceTest::firefoxFallback(instance, 0, false);
        fcitx::EnginePerformanceTest::firefoxFallback(instance, 2, false);
        fcitx::EnginePerformanceTest::firefoxFallback(instance, 1, true);
        for (bool wayland : {false, true})
            for (const char *keys : {"ddaay", "ddaya"})
                for (int arrival : {0, 1, 2})
                    fcitx::EnginePerformanceTest::officeWord(instance, wayland, keys, arrival);
        fcitx::EnginePerformanceTest::officeAppendOrdering(instance, true, "soffice.bin", true);
        fcitx::EnginePerformanceTest::officeAppendOrdering(instance, false, "soffice.bin", false);
        fcitx::EnginePerformanceTest::officeAppendOrdering(instance, true, "skey-test", false);
        fcitx::EnginePerformanceTest::electronBackspace(instance);
        fcitx::EnginePerformanceTest::nativeBackspaceUnchanged(instance, true, "google-chrome", true);
        fcitx::EnginePerformanceTest::nativeBackspaceUnchanged(instance, true, "skey-test", false);
        fcitx::EnginePerformanceTest::nativeBackspaceUnchanged(instance, false, "antigravity-ide", true);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::addressBarIdentity(instance, false);
        fcitx::EnginePerformanceTest::addressBarIdentity(instance, true);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::run(instance, false);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::runNative(instance, false);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::run(instance, true);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::runNative(instance, true);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::cancelPending(instance);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::boundedRepair(instance);
    }
    std::cout << "Engine performance: " << checks << " checks passed\n";
}
