#include "engine.h"
#include "process_markers.h"
#include "input_timing.h"
#include "uinput_delete_ack.h"
#include "a11y_work_policy.h"
#include "x11_app_name.h"
#include <fcitx-utils/event.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/focusgroup.h>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <csignal>
#include <sys/wait.h>
#include <limits>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>

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
    // Frontend-keyed policies (input_field_policy.h) need the real one —
    // e.g. Tabby and VS Code talk D-Bus.
    const char *frontend() const override { return frontendName.c_str(); }
    std::string frontendName = "test";
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
    static void uinputPendingTransport(Instance &instance, bool settling) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager());
        input.setFocusGroup(&group);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "skey-test";
        state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.uinputRequest_ = std::make_shared<skey::UinputInjector::Request>();
        state.pendingUinputCommit_ = "õ";
        state.uinputDeleting_ = true;
        if (settling) {
            state.scheduleUinputSettle(0, 0, 0);
        } else {
            state.uinputSafetyRetried_ = true; // exercise the final timeout
            state.armUinputSafetyTimer();
            state.uinputSafetyTimer_->setTime(now(CLOCK_MONOTONIC));
        }
        auto complete = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
            now(CLOCK_MONOTONIC) + 20000, 1, [&](EventSourceTime *, uint64_t) {
                check(input.commits.empty() && state.pendingUinputCommit_ == "õ" && state.uinputDeleting_,
                      "a timeout must not commit while the worker is still emitting Backspaces");
                state.uinputRequest_->state.store(skey::UinputInjector::State::Complete);
                return true;
            });
        auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
            now(CLOCK_MONOTONIC) + 40000, 1,
            [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
        instance.eventLoop().exec();
        check(input.commits == std::vector<std::string>{"õ"} && !state.uinputDeleting_,
              "a deferred timer resumes exactly once after the worker completes");
    }

    static void uinputFailures(Instance &instance) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager());
        input.setFocusGroup(&group);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "skey-test";
        state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.uinputTestReady_ = false;
        KeyEvent first(&input, Key(FcitxKey_d));
        state.keyEvent(first);
        check(!first.accepted() && state.viet_.getRawInput().empty() && input.commits.empty(),
              "unready Uinput must pass the first key through without changing composition");
        state.uinputTestReady_ = true;
        int calls = 0;
        state.uinputTestSend_ = [&](int, uint32_t, uint32_t) { ++calls; return false; };
        state.viet_.setRawInput("ban"); state.committedLen_ = 3;
        KeyEvent tone(&input, Key(FcitxKey_j));
        state.keyEvent(tone);
        check(calls == 1 && !tone.accepted() && !tone.filtered() &&
              !state.uinputDeleting_ && !state.uinputSafetyTimer_ &&
              state.pendingUinputCommit_.empty() && input.commits.empty(),
              "rejected suffix deletion passes the key through without a replacement commit");
        state.surroundingCommit("go", "gõ");
        check(calls == 2 && !state.uinputDeleting_ && !state.uinputSafetyTimer_ && input.commits.empty(),
              "rejected surrounding fallback must not commit over undeleted text");
        state.uinputRequest_ = std::make_shared<skey::UinputInjector::Request>();
        state.uinputRequest_->state.store(skey::UinputInjector::State::Failed);
        state.uinputBsOutstanding_ = 2;
        state.expectedUinputBackspaces_ = 1;
        state.pendingUinputCommit_ = "õ";
        state.uinputDeleting_ = true;
        state.scheduleUinputSettle(0, 0, 0);
        state.finishUinputSettle(0, 0);
        check(input.commits.empty() && !state.uinputDeleting_ && !state.uinputCommitTimer_ &&
              state.pendingUinputCommit_.empty() && state.uinputBsOutstanding_ == 0,
              "partial failure cancels the commit and unsent outstanding count");
        KeyEvent late(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(late);
        check(late.accepted(), "late partial-write loopback retains the existing grace");
        check(!state.sendBackspaceUinput(2), "failed batch must drain before the next replacement");
        KeyEvent draining(&input, Key(FcitxKey_j));
        state.keyEvent(draining);
        check(!draining.accepted() && state.viet_.getRawInput().empty() && calls == 2,
              "new physical keys pass through while cancelled Backspaces drain");
        state.uinputLateBsDeadlineUsec_ = now(CLOCK_MONOTONIC) - 1;
        KeyEvent user(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(user);
        check(!user.accepted(), "unsent cancelled keys must not swallow future user Backspace");
    }

    // Queued keys replay behind a commit-dispatch timer (replaySettledKeys,
    // 2026-10-28) that only the event loop fires — run it inline.
    static void drainSettledReplay(SKeyState &state) {
        for (int i = 0; i < 8 && (state.settledReplayTimer_ || !state.settledKeys_.empty()); ++i) {
            state.settledReplayTimer_.reset();
            state.replaySettledKeys();
        }
    }
    static void captureUinput(SKeyState &state, int fd) {
        auto owned = std::shared_ptr<int>(new int(fd), [](int *p) { close(*p); delete p; });
        state.uinputTestSend_ = [fd, owned](int count, uint32_t flags, uint32_t pace) {
            const uint32_t request[] = {static_cast<uint32_t>(count), flags, 0, pace};
            return send(fd, request, sizeof(request), MSG_NOSIGNAL) == sizeof(request);
        };
    }
    static void omniboxSurrounding(Instance &instance, bool native, bool wayland,
                                   bool nativeSuccess = false) {
        class Editor : public TestInput {
        public:
            using TestInput::TestInput;
            std::string text = "chaexample1";
            bool selected = true;
            void backspace() {
                if (selected) { text.resize(3); selected = false; }
                else if (!text.empty()) text.pop_back();
            }
        protected:
            void commitStringImpl(const std::string &value) override {
                TestInput::commitStringImpl(value);
                text += value;
            }
        };
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(SKeyOutputMode::Auto);
        engine.config_.chromiumAddressBarMode.setValue(SKeyChromiumAddressBarMode::SurroundingText);
        if (nativeSuccess)
            engine.config_.chromiumAddressBarMode.setValue(SKeyChromiumAddressBarMode::Native);
        engine.config_.enableNative.setValue(native);
        engine.config_.replaceUinputWithNative.setValue(native);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        Editor input(instance.inputContextManager(), "google-chrome");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(wayland ? CapabilityFlags(0x72) | CapabilityFlag::Url : CapabilityFlags(0x72));
        if (!wayland) input.setCursorRect(Rect(200, 54, 201, 72));
        input.surroundingText().setText(input.text, 3, 11);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = state.cachedIsWebKitGtk_ = 0;
        state.cachedWebKitProgram_ = "google-chrome";
        check(state.effectiveMode() == (nativeSuccess ? SKeyOutputMode::Native : SKeyOutputMode::SurroundingText),
              "omnibox override preserves the requested output mode");
        check(state.useNativeBackspaceTransport() == native,
              "omnibox Surrounding Text honors replacement transport preference");
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "isolate omnibox transport without desktop injection");
        captureUinput(state, sockets[0]);
        if (nativeSuccess) {
            state.nativeTestSend_ = std::move(state.uinputTestSend_);
            state.uinputTestSend_ = [](int, uint32_t, uint32_t) {
                check(false, "X11 Native must not fall back to Uinput");
                return false;
            };
        }
        state.viet_.setRawInput(native && !nativeSuccess ? "chaf" : "cha");
        state.committedLen_ = 3;
        if (native && !nativeSuccess) {
            // An unstarted native injector rejects deterministically. It
            // must neither fall back to the Uinput request capture nor
            // schedule a timeout that commits over unchanged text.
            state.scheduleAddrBarReplacement(1, "à", 3, 0, 0, "chà", false, "cha");
            uint32_t request[4];
            check(recv(sockets[1], request, sizeof(request), 0) < 0 && errno == EAGAIN,
                  "Native preference must not send through Uinput in Surrounding Text");
            check(state.nativeFailed_ && !state.uinputDeleting_ && !state.uinputSafetyTimer_ &&
                      state.pendingUinputCommit_.empty() && input.commits.empty() &&
                      state.viet_.getComposed() == "cha" && !state.addrBarHadFirstWord_,
                  "rejected deletion cancels replacement and retains the actual on-screen word");
            auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + 150000, 1,
                [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
            instance.eventLoop().exec();
            check(input.text == "chaexample1" && input.commits.empty(),
                  "no timeout may append chà when Backspaces were never sent");
        } else {
            KeyEvent tone(&input, Key(FcitxKey_f));
            state.keyEvent(tone);
            uint32_t request[4] = {};
            check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request) && request[0] == 5,
                  "first-word replacement sends selection dismissal, three deletions and one anchor");
            KeyEvent append(&input, Key(FcitxKey_o));
            state.keyEvent(append);
            for (uint32_t i = 0; i < request[0]; ++i) {
                KeyEvent deletion(&input, Key(FcitxKey_BackSpace));
                state.keyEvent(deletion);
                check(deletion.accepted() == (i + 1 == request[0]),
                      "only the omnibox synchronization anchor is consumed");
                if (!deletion.accepted()) input.backspace();
            }
            state.finishUinputSettle(0, 0);
            if (state.settledReplayTimer_) {
                auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
                    now(CLOCK_MONOTONIC) + 80000, 1,
                    [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
                instance.eventLoop().exec();
            }
            if (input.text != "chào" || state.uinputDeleting_)
                std::cerr << "Omnibox wayland=" << wayland << " text='" << input.text
                          << "' pending='" << state.pendingUinputCommit_
                          << "' buffered=" << state.bufferedUinputKeys_.size() << "\n";
            check(input.text == "chào" && !state.uinputDeleting_,
                  "omnibox full replacement plus buffered o renders chào, not chachào");
        }
        // Transport policy must remain local to the omnibox.
        input.setCapabilityFlags(CapabilityFlags(0x72));
        input.setCursorRect(Rect(200, 400, 201, 420));
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.addrBarUiVerdictAtUsec_ = 0;
        // Native X11 Chromium page editors in Auto ride the native transport
        // on their own (editRoute, 2026-10-28) — independent of the omnibox
        // preference.  Pin Surrounding Text to test the locality itself.
        check(state.useNativeBackspaceTransport() == !wayland,
              "Auto: native X11 Chromium web content rides the native transport, Wayland does not");
        engine.config_.outputMode.setValue(SKeyOutputMode::SurroundingText);
        state.modeCacheValid_ = false;
        check(!state.useNativeBackspaceTransport(), "web content keeps its existing Surrounding Text transport");
        close(sockets[1]);
    }
    static void webkitToolkitDetection(Instance &instance) {
        // Exercise /proc detection using a mapped toolkit-named file in our
        // own process; no dependency on an installed WebKit or live desktop.
        const auto program = std::filesystem::read_symlink("/proc/self/exe").filename().string();
        SKeyEngine engine(&instance, false);
        TestInput input(instance.inputContextManager(), program);
        auto &state = *input.propertyFor(&engine.factory_);
        check(!state.webkitGtkSurroundingFallback(), "ordinary process must not be classified as WebKitGTK");
        char directory[] = "/tmp/skey-webkit-maps-XXXXXX";
        check(mkdtemp(directory) != nullptr, "create isolated toolkit mapping fixture");
        const std::string path = std::string(directory) + "/libwebkit2gtk-test.so";
        int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        check(fd >= 0 && ftruncate(fd, 4096) == 0, "create toolkit mapping fixture");
        void *mapping = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        check(mapping != MAP_FAILED, "map toolkit fixture");
        state.cachedIsWebKitGtk_ = -1;
        // The engine-level WebKitGTK verdict lives 5s; simulate its expiry.
        engine.programTraits_.clear();
        check(state.webkitGtkSurroundingFallback(), "loaded toolkit is recognized without application-name exceptions");
        munmap(mapping, 4096);
        unlink(path.c_str());
        rmdir(directory);
        state.cachedIsWebKitGtk_ = -1;
        engine.programTraits_.clear();
        check(!state.webkitGtkSurroundingFallback(), "unmapped toolkit must not leave a positive detector result after refresh");
    }
    static void webkitSurrounding(Instance &instance, bool wayland, bool perApp,
                                   const std::string &keys) {
        class Editor : public TestInput {
        public:
            using TestInput::TestInput;
            const char *frontend() const override { return "dbus"; }
            std::string text = "xin ";
            unsigned pending = 0;
            void backspace() {
                check(!text.empty(), "WebKit replacement preserves prefix");
                size_t last = text.size() - 1;
                while (last && (static_cast<unsigned char>(text[last]) & 0xc0) == 0x80) --last;
                text.resize(last);
            }
        protected:
            void commitStringImpl(const std::string &value) override {
                commits.push_back(value);
                text += value;
            }
            void forwardKeyImpl(const ForwardKeyEvent &event) override {
                TestInput::forwardKeyImpl(event);
                if (!event.isRelease()) ++pending;
            }
        };
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(perApp ? SKeyOutputMode::Auto : SKeyOutputMode::SurroundingText);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        Editor input(instance.inputContextManager(), "unknown-webkit-editor");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlags(0xe001010060ULL));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = state.cachedWebKitProgram_ = "unknown-webkit-editor";
        state.cachedIsWebKitGtk_ = 1; // toolkit detection, independent of application identity
        state.cachedIsChromium_ = state.cachedIsTerminalApp_ = state.cachedIsFirefoxOrSnap_ = 0;
        state.hasAppModeOverride_ = perApp;
        state.appModeOverride_ = SKeyOutputMode::SurroundingText;
        auto press = [&](KeySym sym) {
            auto cursor = utf8::length(input.text);
            input.surroundingText().setText(input.text, cursor, cursor);
            input.updateSurroundingText();
            KeyEvent event(&input, Key(sym));
            state.keyEvent(event);
            if (!event.accepted() && sym == FcitxKey_BackSpace) input.backspace();
            check(input.deletions == 0, "WebKit retrieval must not imply deletion support");
        };
        check(state.effectiveMode() == SKeyOutputMode::SurroundingText, "explicit Surrounding Text stays selected");
        for (size_t i = 0; i < keys.size(); ++i) {
            press(static_cast<KeySym>(keys[i]));
            if (state.hasDeferredCommitPending()) {
                check(input.pending > 0 && input.commits.back() == (keys == "chafo" ? "a" : "o"),
                      "accent waits for Backspace instead of immediately duplicating a");
                if (keys == "chafo" && i + 1 < keys.size()) press(static_cast<KeySym>(keys[++i]));
                while (input.pending) { --input.pending; input.backspace(); }
                state.finishDeferredCommit(0);
            }
        }
        check(input.text == "xin chào", "WebKit Surrounding Text renders chào for both tone orders");
        press(FcitxKey_BackSpace);
        while (input.pending) { --input.pending; input.backspace(); }
        check(input.text == "xin chà", "WebKit composing Backspace deletes exactly once");
        state.viet_.reset();
        press(FcitxKey_BackSpace);
        while (input.pending) { --input.pending; input.backspace(); }
        check(input.text == "xin ch", "WebKit idle Backspace deletes exactly once");
        // Auto's decision must stay Surrounding Text, with or without Preedit.
        state.hasAppModeOverride_ = false;
        engine.config_.outputMode.setValue(SKeyOutputMode::Auto);
        for (uint64_t caps : {0x10072ULL, 0xe001010060ULL}) {
            input.setCapabilityFlags(CapabilityFlags(caps));
            state.modeCacheValid_ = false;
            check(state.effectiveMode() == SKeyOutputMode::SurroundingText, "WebKit fix must not change Auto mode selection");
        }
        state.cachedIsWebKitGtk_ = 0;
        check(state.useNativeSurroundingApi(), "non-WebKit native deletion policy is unchanged");
    }
    static void nativeAddressBarChurn(Instance &instance, SKeyOutputMode mode,
                                      bool settling) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::Preedit);
        input.setCursorRect(Rect(239, 54, 240, 72));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome-stable";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = mode;
        state.modeCacheValid_ = true;
        state.viet_.setRawInput("chaf");
        state.committedLen_ = 3;
        state.armAddrBarCycle();
        state.uinputDeleting_ = true;
        state.uinputSettling_ = settling;
        state.pendingUinputCommit_ = "chà";
        state.expectedUinputBackspaces_ = settling ? 0 : 4;
        state.seenUinputBackspaces_ = settling ? 0 : 2;
        state.uinputBsOutstanding_ = settling ? 0 : 3;
        state.deactivate();
        check(state.pendingUinputCommit_ == "chà" && state.uinputDeleting_,
              "omnibox focus churn retains pending replacement for both transports");
        check(state.uinputSettling_ == settling &&
              state.expectedUinputBackspaces_ == (settling ? 0 : 4) &&
              state.seenUinputBackspaces_ == (settling ? 0 : 2),
              "omnibox churn retains deletion counters and settle state");
        check(state.viet_.getRawInput() == "chaf",
              "omnibox churn preserves composition until replacement commits");
        state.addrBarCycleTimer_.reset();
        state.resetForCellChange();
    }
    static void replacementMenus(Instance &instance) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome");
        input.setFocusGroup(&group);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome";
        state.cachedIsChromium_ = 1;
        engine.config_.enableNative.setValue(true);
        for (bool replace : {false, true}) {
            engine.config_.replaceUinputWithNative.setValue(replace);
            for (bool address : {false, true}) {
                input.setCapabilityFlags(address ? CapabilityFlag::Url : CapabilityFlags{});
                engine.config_.outputMode.setValue(SKeyOutputMode::Uinput);
                engine.config_.chromiumAddressBarMode.setValue(SKeyChromiumAddressBarMode::Uinput);
                state.hasAppModeOverride_ = false;
                state.modeCacheValid_ = false;
                check(state.effectiveMode() == (replace ? SKeyOutputMode::Native : SKeyOutputMode::Uinput),
                      "saved Uinput config resolves to selected transport");
                state.showModeMenu();
                auto list = input.inputPanel().candidateList();
                check(list && list->size() ==
                          (address ? (replace ? 6 : 7) : (replace ? 5 : 6)),
                      "replacement menu keeps the Select slot and the disable action");
                check(list->candidate(list->size() - 1).text().toString().find("0. ") == 0,
                      "both menus expose their disable action as zero");
                check(list->candidate(1).text().toString() == (replace ? "2. Native" : "2. Uinput"),
                      "Native occupies Uinput menu position");
                check(list->cursorIndex() == 1, "saved transport keeps correct highlighted row");
                if (replace) for (int i = 0; i < list->size(); ++i)
                    check(list->candidate(i).text().toString().find("Uinput") == std::string::npos,
                          "replacement menu has no Uinput label");
                KeyEvent select(&input, Key(FcitxKey_2));
                state.keyEvent(select);
                check(select.accepted() && !state.modeMenuActive_, "numeric key selects visible candidate");
                state.modeCacheValid_ = false;
                check(state.effectiveMode() == (replace ? SKeyOutputMode::Native : SKeyOutputMode::Uinput),
                      "numeric menu selection uses displayed transport");
                for (auto zero : {FcitxKey_0, FcitxKey_KP_0}) {
                    state.showModeMenu();
                    KeyEvent exclude(&input, Key(zero));
                    state.keyEvent(exclude);
                    check(exclude.accepted() && !state.modeMenuActive_, "zero selects the menu disable action");
                    if (address) {
                        check(engine.config_.chromiumAddressBarMode.value() == SKeyChromiumAddressBarMode::NoVietnamese,
                              "zero disables Vietnamese in address bars");
                        check(!state.appExcluded_, "address bar zero does not exclude the application");
                    } else {
                        check(state.appExcluded_ == (zero == FcitxKey_0), "zero toggles application exclusion");
                    }
                }
            }
        }
        engine.config_.enableNative.setValue(false);
        engine.config_.replaceUinputWithNative.setValue(false);
    }

    static void configureGnomeOmnibox(SKeyEngine &engine, bool native) {
        engine.config_.enableNative.setValue(native);
        engine.config_.chromiumAddressBarMode.setValue(
            native ? SKeyChromiumAddressBarMode::Native : SKeyChromiumAddressBarMode::Uinput);
    }

    static void captureGnomeTransport(SKeyState &state, int fd, bool native) {
        captureUinput(state, fd);
        check(state.useNativeBackspaceTransport() == native,
              "GNOME omnibox selects the requested replacement transport");
        if (native) {
            state.nativeTestSend_ = std::move(state.uinputTestSend_);
            state.uinputTestSend_ = [](int, uint32_t, uint32_t) {
                check(false, "Native must not fall back to Uinput");
                return false;
            };
        }
    }

    static void gnomeOmniboxReplacement(Instance &instance, bool native = false) {
        class Omnibox : public TestInput {
        public:
            using TestInput::TestInput;
            std::string text;
            size_t typedBytes = 0;
            bool selected = false;
            void backspace() {
                if (selected) { text.resize(typedBytes); selected = false; }
                else if (!text.empty())
                    text.resize(utf8::ncharByteLength(text.begin(), utf8::length(text) - 1));
            }
        protected:
            void commitStringImpl(const std::string &value) override {
                TestInput::commitStringImpl(value);
                text += value;
            }
            void forwardKeyImpl(const ForwardKeyEvent &event) override {
                TestInput::forwardKeyImpl(event);
                if (!event.isRelease()) text += Key::keySymToUTF8(event.rawKey().sym());
            }
        };
        for (const std::string &prefix : {std::string{}, std::string("tìm ")})
            for (bool selected : {false, true})
                for (bool reversed : {false, true}) {
                    SKeyEngine engine(&instance, false);
                    configureGnomeOmnibox(engine, native);
                    FocusGroup group("wayland:test", instance.inputContextManager());
                    Omnibox input(instance.inputContextManager(), "gnome-shell");
                    input.setFocusGroup(&group);
                    input.setCursorRect(Rect(344, 86, 345, 104));
                    input.setCapabilityFlags(CapabilityFlag::Preedit); // real relay caps=0x12, no Url
                    input.text = prefix + "cha" + (selected ? "example" : "");
                    input.typedBytes = prefix.size() + 3;
                    input.selected = selected;
                    const unsigned start = utf8::length(prefix) + 3;
                    const unsigned end = utf8::length(input.text);
                    input.surroundingText().setText(input.text, reversed ? end : start, reversed ? start : end);
                    auto &state = *input.propertyFor(&engine.factory_);
                    state.cachedProgram_ = "gnome-shell";
                    state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
                    state.viet_.setRawInput("cha");
                    state.committedLen_ = 3;
                    int sockets[2];
                    check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
                          "isolate GNOME omnibox Backspace transport");
                    captureGnomeTransport(state, sockets[0], native);
                    KeyEvent tone(&input, Key(FcitxKey_f));
                    state.keyEvent(tone);
                    uint32_t request[4] = {};
                    check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request), "GNOME replacement sends a replacement");
                    check(request[0] == (selected ? 3u : 2u) && request[1] == 0 && request[3] == 5000,
                          "GNOME replacement dismisses only active autofill and paces Backspaces");
                    check(!state.uinputShiftSelect_, "explicit replacement mode stays on Backspace transport");
                    if (reversed) {
                        KeyEvent append(&input, Key(FcitxKey_o));
                        state.keyEvent(append);
                        check(append.accepted(), "fast append waits for GNOME replacement");
                    }
                    for (uint32_t i = 0; i < request[0]; ++i) {
                        KeyEvent deletion(&input, Key(FcitxKey_BackSpace));
                        state.keyEvent(deletion);
                        check(deletion.accepted() == (i + 1 == request[0]), "only the final GNOME Backspace is the anchor");
                        if (!deletion.accepted()) input.backspace();
                    }
                    state.finishUinputSettle(0, 0);
                    drainSettledReplay(state);
                    if (reversed) {
                        check(state.settledKeys_.empty() && state.bufferedUinputKeys_.empty(),
                              "GNOME replacement drains the queued append exactly once");
                    } else {
                        KeyEvent append(&input, Key(FcitxKey_o));
                        state.keyEvent(append);
                        if (!append.accepted()) input.text += "o";
                        drainSettledReplay(state);
                    }
                    check(input.text == prefix + "chào", "GNOME replacement produces chào without leftover a or lost prefix");
                    state.resetForCellChange();
                    close(sockets[1]);
                }
        // A stale unrelated snapshot, a wide web-editor caret, or a
        // non-relay program must not receive the GNOME-only extra BS.
        for (int scenario = 0; scenario < 3; ++scenario) {
            SKeyEngine engine(&instance, false);
            configureGnomeOmnibox(engine, native);
            engine.config_.outputMode.setValue(native ? SKeyOutputMode::Native : SKeyOutputMode::Uinput);
            const std::string program = scenario == 2 ? "code" : "gnome-shell";
            FocusGroup group("wayland:test", instance.inputContextManager());
            TestInput input(instance.inputContextManager(), program);
            input.setFocusGroup(&group);
            input.setCursorRect(Rect(344, 86, scenario == 1 ? 424 : 345, 104));
            input.setCapabilityFlags(CapabilityFlag::Preedit);
            input.surroundingText().setText(scenario == 0 ? "chxexample" : "chaexample", 3, 10);
            auto &state = *input.propertyFor(&engine.factory_);
            state.cachedProgram_ = program;
            state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
            state.viet_.setRawInput("cha");
            state.committedLen_ = 3;
            int sockets[2];
            check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0, "isolate negative GNOME cases");
            captureGnomeTransport(state, sockets[0], native);
            KeyEvent tone(&input, Key(FcitxKey_f));
            state.keyEvent(tone);
            uint32_t request[4] = {};
            check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request) && request[0] == 2,
                  "unrelated snapshots and non-target fields retain exact deletion count");
            check(request[3] == (scenario == 0 ? 5000u : 1000u), "non-relay and wide fields retain existing pace");
            state.resetForCellChange();
            close(sockets[1]);
        }
    }

    static void gnomeOmniboxAppendAfterTimeout(Instance &instance, bool native = false) {
        class Omnibox : public TestInput {
        public:
            using TestInput::TestInput;
            std::string text, pending;
            int droppedRaw = 0;
            void raw(const Key &key) {
                // The relay can deliver a physical/replayed key before the
                // pending IM commit has reached Chrome, where it disappears.
                if (!pending.empty()) ++droppedRaw;
                else text += Key::keySymToUTF8(key.sym());
            }
            void flush() {
                text += pending;
                pending.clear();
            }
        protected:
            void commitStringImpl(const std::string &value) override {
                TestInput::commitStringImpl(value);
                pending += value;
            }
            void forwardKeyImpl(const ForwardKeyEvent &event) override {
                TestInput::forwardKeyImpl(event);
                if (!event.isRelease()) raw(event.rawKey());
            }
        };
        for (const std::string &prefix : {std::string("chào "), std::string("chào các ")})
            for (bool queued : {false, true}) {
                SKeyEngine engine(&instance, false);
                configureGnomeOmnibox(engine, native);
                FocusGroup group("wayland:test", instance.inputContextManager());
                Omnibox input(instance.inputContextManager(), "gnome-shell");
                input.setFocusGroup(&group);
                input.setCursorRect(Rect(320, 86, 321, 104));
                input.setCapabilityFlags(CapabilityFlag::Preedit);
                input.text = prefix + "ca";
                const unsigned cursor = utf8::length(input.text);
                input.surroundingText().setText(input.text, cursor, cursor);
                auto &state = *input.propertyFor(&engine.factory_);
                state.cachedProgram_ = "gnome-shell";
                state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
                state.viet_.setRawInput("ca");
                state.committedLen_ = 2;
                int sockets[2];
                check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
                      "isolate GNOME subsequent-word replacement");
                captureGnomeTransport(state, sockets[0], native);
                KeyEvent tone(&input, Key(FcitxKey_s));
                state.keyEvent(tone);
                uint32_t request[4] = {};
                check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request) && request[0] == 2,
                      "second-word tone uses one deletion and an anchor");
                for (uint32_t i = 0; i < request[0]; ++i) {
                    KeyEvent deletion(&input, Key(FcitxKey_BackSpace));
                    state.keyEvent(deletion);
                    if (!deletion.accepted()) input.text.pop_back(); // ASCII a
                }
                if (queued) {
                    KeyEvent append(&input, Key(FcitxKey_c));
                    state.keyEvent(append);
                    check(append.accepted(), "final c queues while GNOME waits for deletion");
                }
                // Advance the unconfirmed deadline deterministically while
                // leaving the surrounding snapshot one edit behind, as in
                // the reported GNOME trace (cursor=anchor=7).
                check(state.uinputDeleteAck_.start(prefix + "ca", cursor, cursor, "ca", 1,
                          now(CLOCK_MONOTONIC) - 200000, 150000), "simulate expired GNOME deletion acknowledgement");
                state.finishUinputSettle(0, 0);
                drainSettledReplay(state);
                if (!queued) {
                    KeyEvent append(&input, Key(FcitxKey_c));
                    state.keyEvent(append);
                    if (!append.accepted()) input.raw(append.key());
                    drainSettledReplay(state);
                }
                input.flush();
                check(input.text == prefix + "các" && input.droppedRaw == 0,
                      "GNOME replacement preserves the final c after an unconfirmed tone replacement");
                check(input.commits == std::vector<std::string>{"á", "c"},
                      "GNOME replacement and append use one ordered commit channel");
                state.resetForCellChange();
                close(sockets[1]);
            }
    }

    static void gnomeOmniboxEditing(Instance &instance, bool native = false) {
        class Editor : public TestInput {
        public:
            using TestInput::TestInput;
            std::string text;
            unsigned cursor = 0, anchor = 0;
            size_t byte(unsigned pos) const {
                return utf8::ncharByteLength(text.begin(), pos);
            }
            void insert(const std::string &value) {
                const unsigned begin = std::min(cursor, anchor), end = std::max(cursor, anchor);
                text.replace(byte(begin), byte(end) - byte(begin), value);
                cursor = anchor = begin + utf8::length(value);
            }
            void raw(const Key &key) {
                const auto sym = key.sym();
                if (key.states().test(KeyState::Ctrl)) {
                    if (sym == FcitxKey_a || sym == FcitxKey_l) {
                        anchor = 0;
                        cursor = utf8::length(text);
                    }
                    return;
                }
                if (sym == FcitxKey_BackSpace || sym == FcitxKey_Delete) {
                    if (cursor != anchor) insert("");
                    else if (sym == FcitxKey_BackSpace && cursor > 0) {
                        anchor = cursor - 1;
                        insert("");
                    } else if (sym == FcitxKey_Delete && cursor < utf8::length(text)) {
                        anchor = cursor + 1;
                        insert("");
                    }
                } else insert(Key::keySymToUTF8(sym));
            }
            void publish() {
                surroundingText().setText(text, cursor, anchor);
                updateSurroundingText();
            }
        protected:
            void commitStringImpl(const std::string &value) override {
                TestInput::commitStringImpl(value);
                insert(value);
            }
            void forwardKeyImpl(const ForwardKeyEvent &event) override {
                TestInput::forwardKeyImpl(event);
                if (!event.isRelease()) raw(event.rawKey());
            }
        };
        enum class Edit {
            OverwriteSelection,
            ClearWithBackspace,
            DeleteMultipleWords,
            DeleteAll,
            ClearWithDelete,
            ClearAddressBar,
            DeleteLastLetter,
            RepeatEdits,
        };
        for (const auto scenario : {Edit::OverwriteSelection, Edit::ClearWithBackspace,
                                    Edit::DeleteMultipleWords, Edit::DeleteAll,
                                    Edit::ClearWithDelete, Edit::ClearAddressBar,
                                    Edit::DeleteLastLetter, Edit::RepeatEdits}) {
            SKeyEngine engine(&instance, false);
            configureGnomeOmnibox(engine, native);
            // These cases test editing, not the acknowledgement deadline;
            // the preceding regression covers stale snapshots and timeouts.
            engine.config_.autoDelay.setValue(false);
            FocusGroup group("wayland:test", instance.inputContextManager());
            Editor input(instance.inputContextManager(), "gnome-shell");
            input.setFocusGroup(&group);
            input.setCursorRect(Rect(320, 86, 321, 104));
            input.setCapabilityFlags(CapabilityFlag::Preedit);
            auto &state = *input.propertyFor(&engine.factory_);
            state.cachedProgram_ = "gnome-shell";
            state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
            int sockets[2];
            check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
                  "isolate GNOME edit/retype session");
            captureGnomeTransport(state, sockets[0], native);
            input.publish();
            auto press = [&](Key key, bool publish = true) {
                KeyEvent event(&input, key);
                state.keyEvent(event);
                if (!event.accepted()) input.raw(key);
                uint32_t request[4];
                while (recv(sockets[1], request, sizeof(request), 0) == sizeof(request)) {
                    for (uint32_t i = 0; i < request[0]; ++i) {
                        KeyEvent deletion(&input, Key(FcitxKey_BackSpace));
                        state.keyEvent(deletion);
                        if (!deletion.accepted()) input.raw(deletion.key());
                    }
                    state.finishUinputSettle(0, 0);
                }
                // Plain keys can queue behind the previous commit's
                // dispatch timer too — drain after every press.
                drainSettledReplay(state);
                if (publish) input.publish();
            };
            auto type = [&](const std::string &keys) {
                for (unsigned char ch : keys) press(Key(static_cast<KeySym>(ch)));
            };
            auto expect = [&](const std::string &text) {
                if (input.text != text)
                    std::cerr << "GNOME edit scenario=" << static_cast<int>(scenario) << " actual='" << input.text
                              << "' expected='" << text << "'\n";
                check(input.text == text, "GNOME edit/retype retains exact visible Vietnamese text");
                check(!state.uinputDeleting_ && state.settledKeys_.empty() && state.bufferedUinputKeys_.empty(),
                      "GNOME edit/retype drains replacement queues");
            };
            type("chafo casc banj");
            expect("chào các bạn");
            switch (scenario) {
            case Edit::OverwriteSelection: // Ctrl+A with a lagging selection snapshot.
                press(Key(FcitxKey_a, KeyState::Ctrl), false);
                type("xin chafo");
                expect("xin chào");
                break;
            case Edit::ClearWithBackspace:
            case Edit::ClearWithDelete:
            case Edit::ClearAddressBar:
                press(Key(scenario == Edit::ClearAddressBar ? FcitxKey_l : FcitxKey_a,
                          KeyState::Ctrl), false);
                press(Key(scenario == Edit::ClearWithDelete ? FcitxKey_Delete : FcitxKey_BackSpace));
                expect("");
                type("chafo casc");
                expect("chào các");
                break;
            case Edit::DeleteMultipleWords:
                for (int i = 0; i < 7; ++i) press(Key(FcitxKey_BackSpace));
                expect("chào ");
                type("casc banj");
                expect("chào các bạn");
                break;
            case Edit::DeleteAll: // Include extra Backspaces on an empty field.
                for (int i = 0; i < 14; ++i) press(Key(FcitxKey_BackSpace));
                expect("");
                type("chafo casc");
                expect("chào các");
                break;
            case Edit::DeleteLastLetter: // Preserve the accented stem.
                press(Key(FcitxKey_BackSpace));
                expect("chào các bạ");
                type("n");
                expect("chào các bạn");
                break;
            case Edit::RepeatEdits: // Reuse the same input context across cycles.
                for (int i = 0; i < 3; ++i) {
                    press(Key(FcitxKey_a, KeyState::Ctrl), false);
                    type("banj");
                    expect("bạn");
                    press(Key(FcitxKey_a, KeyState::Ctrl), false);
                    press(Key(FcitxKey_BackSpace));
                    expect("");
                    type("chafo casc");
                    expect("chào các");
                }
                break;
            }
            state.resetForCellChange();
            close(sockets[1]);
        }
    }

    static void gnomeAddressBarMenu(Instance &instance) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        for (const char *program : {"gnome-shell", "google-chrome"}) {
            TestInput input(instance.inputContextManager(), program);
            input.setFocusGroup(&group);
            // Mutter need not forward Url or SurroundingText capabilities.
            input.setCapabilityFlags(std::string(program) == "gnome-shell"
                                         ? CapabilityFlags{}
                                         : CapabilityFlags(CapabilityFlag::Url));
            input.setCursorRect(Rect(10, 50, 11, 70));
            auto &state = *input.propertyFor(&engine.factory_);
            state.cachedProgram_ = program;
            engine.config_.outputMode.setValue(SKeyOutputMode::SurroundingText);
            state.hasAppModeOverride_ = true;
            state.appModeOverride_ = SKeyOutputMode::SurroundingText;
            for (bool native : {false, true}) {
                engine.config_.enableNative.setValue(native);
                engine.config_.replaceUinputWithNative.setValue(false);
                engine.config_.chromiumAddressBarMode.setValue(SKeyChromiumAddressBarMode::Auto);
                state.modeCacheValid_ = false;
                check(state.addressBarModesAvailable(), "GNOME omnibox exposes address-bar modes");
                if (std::string(program) == "gnome-shell")
                    check(!state.inChromiumAddressBar(), "mode controls preserve GNOME replacement routing");
                check(state.effectiveMode() == SKeyOutputMode::Uinput && state.selectTransportActive(),
                      "GNOME omnibox Auto selects shift-select over general/per-app modes");
                state.showModeMenu();
                check(state.modeMenuForAddressBar_, "GNOME omnibox uses address-bar menu");
                auto list = input.inputPanel().candidateList();
                check(list->candidate(0).text().toString() == "1. Auto (Select)",
                      "GNOME Auto label names the Select transport");
                const int selectIndex = native ? 5 : 4;
                check(list->candidate(selectIndex).text().toString() ==
                          std::to_string(selectIndex + 1) + ". Select",
                      "GNOME omnibox includes numbered Select option");
                KeyEvent select(&input, Key(native ? FcitxKey_6 : FcitxKey_5));
                state.keyEvent(select);
                check(select.accepted() && !state.modeMenuActive_ &&
                          engine.config_.chromiumAddressBarMode.value() == SKeyChromiumAddressBarMode::Select,
                      "GNOME Select shortcut saves address-bar mode");
                state.modeCacheValid_ = false;
                check(state.effectiveMode() == SKeyOutputMode::Uinput && state.selectTransportActive(),
                      "explicit GNOME Select uses uinput shift-select");
                state.showModeMenu();
                check(input.inputPanel().candidateList()->cursorIndex() == selectIndex,
                      "explicit GNOME Select highlights the Select row");
                state.dismissModeMenu();
            }
            for (auto mode : {SKeyChromiumAddressBarMode::Uinput,
                              SKeyChromiumAddressBarMode::SurroundingText,
                              SKeyChromiumAddressBarMode::Preedit,
                              SKeyChromiumAddressBarMode::Native,
                              SKeyChromiumAddressBarMode::NoVietnamese}) {
                engine.config_.chromiumAddressBarMode.setValue(mode);
                check(!state.selectTransportActive(), "explicit GNOME modes do not silently use Select");
            }
            if (std::string(program) == "gnome-shell") {
                input.setCursorRect(Rect(10, 50, 18, 70));
                check(!state.addressBarModesAvailable(), "wide GNOME caret keeps ordinary app routing");
                state.showModeMenu();
                check(!state.modeMenuForAddressBar_, "ordinary GNOME fields keep application menu");
                state.dismissModeMenu();
            }
        }
    }

    // Model keys arriving at Fcitx BEFORE the application. Consuming a real
    // deletion here reproduces chaào/caác regardless of the settle delay.
    static void injectedBackspaceContract(Instance &instance, SKeyOutputMode mode) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        OfficeTextInput input(instance.inputContextManager(), true);
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = mode;
        check(state.useUinputMode(), "both transports share Uinput replacement policy");
        struct Replacement { const char *before; const char *after; int deletes; const char *suffix; const char *tail; };
        for (const auto &r : {Replacement{"cha", "chà", 1, "à", "o "},
                              Replacement{"ca", "cá", 1, "á", "c "},
                              Replacement{"ban", "bạn", 2, "ạn", ""}}) {
            input.text += r.before;
            state.viet_.setRawInput(r.after);
            state.committedLen_ = utf8::length(std::string_view(r.before));
            state.pendingUinputCommit_ = r.suffix;
            state.uinputPendingFinalLen_ = utf8::length(std::string_view(r.after));
            state.expectedUinputBackspaces_ = r.deletes;
            state.seenUinputBackspaces_ = 0;
            state.uinputBsOutstanding_ = r.deletes + 1;
            state.uinputDeleting_ = true;
            state.bsSentAt_ = now(CLOCK_MONOTONIC);
            for (int i = 0; i < r.deletes; ++i) {
                KeyEvent deletion(&input, Key(FcitxKey_BackSpace));
                check(state.handlePendingUinputBackspace(deletion), "injected BS handled by transaction");
                check(!deletion.accepted(), "real injected Backspace must reach app, not be swallowed");
                input.key(deletion.rawKey(), false);
                check(!state.uinputSettling_, "wait for anchor before scheduling commit");
                check(state.viet_.getComposed() == r.after, "injected BS must not pop engine composition");
            }
            const auto afterDeletion = input.text;
            KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
            check(state.handlePendingUinputBackspace(anchor) && anchor.accepted(),
                  "only the sync anchor is consumed");
            check(state.uinputSettling_ && state.uinputBsOutstanding_ == 0,
                  "anchor starts shared settle and clears all outstanding keys");
            state.uinputCommitTimer_.reset();
            state.finishUinputSettle(0, 0);
            input.flushCommits();
            check(input.text == afterDeletion + r.suffix, "replace suffix exactly once after actual deletion");
            KeyEvent userBs(&input, Key(FcitxKey_BackSpace));
            check(!state.handlePendingUinputBackspace(userBs) && !userBs.accepted(),
                  "next user Backspace is not swallowed as an outstanding injected key");
            input.text += r.tail;
        }
        check(input.text == "chào các bạn", "both transports preserve visible chào các bạn");
    }

    static void chromiumFirstShortW(Instance &instance, bool wayland,
                                    bool uinput, bool shortW, char keyChar) {
        SKeyEngine engine(&instance, false);
        const auto mode = uinput ? SKeyOutputMode::Uinput : SKeyOutputMode::SurroundingText;
        engine.config_.outputMode.setValue(mode);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        class FirstInput : public TestInput {
        public:
            explicit FirstInput(InputContextManager &manager) : TestInput(manager, "google-chrome") {}
            bool composedCommit = false;
        protected:
            void commitStringImpl(const std::string &value) override {
                composedCommit = !inputPanel().clientPreedit().empty();
                TestInput::commitStringImpl(value);
            }
        } input(instance.inputContextManager());
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlags(CapabilityFlag::Preedit) | CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = mode;
        state.modeCacheValid_ = true;
        state.viet_.setShortW(shortW);
        KeyEvent event(&input, Key(static_cast<KeySym>(keyChar)));
        state.keyEvent(event);
        const bool transformed = shortW && (keyChar == 'w' || keyChar == 'W');
        const std::string expected = transformed ? (keyChar == 'W' ? "Ư" : "ư") : std::string(1, keyChar);
        const bool raw = !transformed && (uinput || wayland);
        check(state.viet_.getComposed() == expected, "ShortW setting controls the first composed character");
        check(event.accepted() != raw, "only unchanged first characters may pass through raw");
        check(input.commits == (raw ? std::vector<std::string>{} : std::vector<std::string>{expected}),
              "transformed first character reaches the app once instead of forwarding w");
        if (wayland && !uinput && transformed)
            check(input.composedCommit && input.inputPanel().clientPreedit().empty(),
                  "transformed first input opens composition and leaves no preedit behind");
    }

    static void firefoxSuffixTransaction(Instance &instance, bool wayland) {
        // Model the observed editor behavior: requests queued against the same
        // snapshot coalesce before the following commit reaches the client.
        class SnapshotInput : public TestInput {
        public:
            explicit SnapshotInput(InputContextManager &manager) : TestInput(manager, "firefox") {}
            std::string text = "ban";
            int offset = 0;
            unsigned count = 0;
        protected:
            void deleteSurroundingTextImpl(int from, unsigned size) override {
                offset = from;
                count = size;
            }
            void commitStringImpl(const std::string &value) override {
                text.erase(static_cast<int>(text.size()) + offset, count);
                text += value;
                commits.push_back(value);
            }
        };
        SKeyEngine engine(&instance, false);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        SnapshotInput input(instance.inputContextManager());
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        input.surroundingText().setText("ban", 3, 3);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "firefox";
        state.cachedIsFirefoxOrSnap_ = 1;
        state.cachedIsChromium_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.committedLen_ = 3;
        state.surroundingCommit("ban", "bạn");
        if (wayland) {
            auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 30000, 1,
                [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
            instance.eventLoop().exec();
        }
        check(input.text == "bạn" && input.commits == std::vector<std::string>{"ạn"},
              "Firefox suffix replacement survives requests coalesced against one snapshot");
    }

    static void surroundingMeasurement(Instance &instance) {
        SKeyEngine engine(&instance, false);
        TestInput input(instance.inputContextManager());
        auto &state = *input.propertyFor(&engine.factory_);
        const auto start = now(CLOCK_MONOTONIC) - 1000;
        state.surroundingMeasureStartedAt_ = start;
        check(state.surroundingMeasurement_.start("tư", 2, 2, "tư", 1, start),
              "measurement starts from an exact client snapshot");
        input.surroundingText().setText("tư", 2, 2);
        state.mirrorSurroundingDelete(-1, 1);
        check(!state.surroundingMeasurement_.acknowledged(),
              "locally mirrored native deletion is not client acknowledgement");
        input.surroundingText().setText("t", 1, 1);
        input.updateSurroundingText();
        check(state.surroundingMeasurement_.observedLatencyUsec() >= 1000,
              "fresh exact client update measures actual deletion latency");
        input.surroundingText().setText("x", 1, 1);
        input.updateSurroundingText();
        check(!state.surroundingMeasurement_.acknowledged(),
              "conflicting client update revokes measurement");
        state.resetForCellChange();
        check(!state.surroundingMeasurement_.active() && !state.surroundingMeasureStartedAt_,
              "measurement cannot leak across input focus boundaries");
    }

    static void surroundingDeadline(Instance &instance, bool wayland, bool native) {
        SKeyEngine engine(&instance, false);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager());
        input.setFocusGroup(&group);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.scheduleDeferredCommit("old", "", 50000);
        state.resetForCellChange();
        check(!state.deferredCommitTimer_ && state.deferredCommitDeadline_ == 0,
              "focus boundary cancels deferred deadline");
        state.bsRtEwma_ = 100000; // unrelated slow Uinput session
        state.deferredBsSentAt_ = now(CLOCK_MONOTONIC);
        state.scheduleDeferredCommit("à", "ch", 5000, native ? 1 : 0,
                                     native ? "a" : "");
        const auto deadline = state.deferredCommitTimer_->time();
        for (int i = 0; i < 8; ++i) {
            state.surroundingCommit("chà", "chào");
            check(state.deferredCommitTimer_->time() == deadline &&
                      state.deferredCommitDeadline_ == deadline,
                  "queued letters never restart deletion deadline or inherit Uinput latency");
            check(state.deferredNativeDeleteLen_ == (native ? 1 : 0) &&
                      state.deferredDeletedTail_ == (native ? "a" : ""),
                  "queued letters retain native deletion verification state");
        }
        state.flushDeferredCommit();
        check(!state.deferredCommitTimer_ || state.deferredCommitTimer_->time() == deadline,
              "word-boundary flush preserves the original policy deadline");
        auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 30000, 1,
            [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
        instance.eventLoop().exec();
        check(input.commits == std::vector<std::string>{"ào"} &&
                  state.deferredCommitDeadline_ == 0,
              "coalesced suffix commits once at original deadline, not the 100ms Uinput delay");
    }

    static void waylandRepeatedLetter(Instance &instance, bool replay, bool timestamped) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        input.surroundingText().setText("đa", 2, 2);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.modeCacheValid_ = true;
        state.viet_.setRawInput("dda");
        state.committedLen_ = 2;
        state.addrBarLastTriggerKey_ = FcitxKey_a;
        state.addrBarTriggerKeyTime_ = timestamped ? 1000 : 0;
        state.addrBarGuardArmedUsec_ = now(CLOCK_MONOTONIC);
        state.addrBarTriggerDeadline_ = now(CLOCK_MONOTONIC) + 100000;
        // The fresh repeat is the field's first Chromium web replacement,
        // which rides the uinput transport: give it one (without it the
        // injector rejects the request and the word resets — the guard
        // under test is never reached).
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "isolate the repeated-letter replacement transport");
        captureUinput(state, sockets[0]);
        KeyEvent event(&input, Key(FcitxKey_a), false,
                       timestamped ? (replay ? 1000 : 1020) : 0);
        state.keyEvent(event);
        check(state.viet_.getComposed() == (!replay && timestamped ? "đâ" : "đa"),
              "Wayland fresh repeat composes aa; identical or missing timestamps retain guard");
        state.resetForCellChange();
        close(sockets[1]);
    }

    static void x11AckBeforeAnchor(Instance &instance, bool firefox) {
        SKeyEngine engine(&instance, false);
        engine.config_.autoDelay.setValue(true);
        FocusGroup group("x11:test", instance.inputContextManager());
        const std::string program = firefox ? "firefox" : "google-chrome-stable";
        TestInput input(instance.inputContextManager(), program);
        input.setFocusGroup(&group);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        state.cachedIsChromium_ = firefox ? 0 : 1;
        state.cachedIsFirefoxOrSnap_ = firefox ? 1 : 0;
        state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        state.uinputDeleting_ = true;
        state.expectedUinputBackspaces_ = state.seenUinputBackspaces_ = 1;
        state.pendingUinputCommit_ = "ự";
        state.uinputPendingFinalLen_ = 2;
        const auto stamp = now(CLOCK_MONOTONIC);
        state.bsSentAt_ = stamp - 20000;
        check(state.uinputDeleteAck_.start("tư", 2, 2, "tư", 1, state.bsSentAt_),
              "prepare deletion whose app confirmation precedes anchor");
        state.uinputDeleteAck_.observe(true, "t", 1, 1, stamp - 12000);
        KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(anchor);
        check(anchor.accepted() && input.commits.empty() && state.uinputSettling_ &&
                  state.uinputCommitTimer_->time() <= now(CLOCK_MONOTONIC),
              "elapsed acknowledgement guard adds no second 8ms sleep after X11 anchor");
        state.finishUinputSettle(0, 0);
        check(input.commits == std::vector<std::string>{"ự"},
              "early acknowledgement still waits for anchor and commits once");
        state.resetForCellChange();
    }

    static void x11BrowserFirstSettle(Instance &instance) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::Preedit);
        input.setCursorRect(Rect(700, 480, 700, 508));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome-stable";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        state.lastActivateUsec_ = now(CLOCK_MONOTONIC);
        const auto appKey = skey::appDelayKey("google-chrome-stable", false);
        for (int replacement = 0; replacement < 3; ++replacement) {
            if (replacement == 2) state.resetForCellChange();
            state.viet_.setRawInput("gox");
            state.committedLen_ = 1;
            state.uinputDeleting_ = true;
            state.expectedUinputBackspaces_ = state.seenUinputBackspaces_ = 1;
            state.uinputBsOutstanding_ = 1;
            state.pendingUinputCommit_ = "õ";
            state.uinputPendingFinalLen_ = 2;
            state.bsRtEwma_ = 1000;
            state.bsSentAt_ = now(CLOCK_MONOTONIC) - 1000;
            KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
            state.keyEvent(anchor);
            check(engine.appDelayStats_.at(appKey).lastSleepUsec ==
                      (replacement == 1 ? 30000u : 50000u),
                  "Chrome first-focus guard is paid once; normal renderer floor remains; boundary rearms it");
            state.finishUinputSettle(0, 0);
            check(state.x11BrowserFocusSettled_, "completed Chrome settle ends first-focus warmup");
        }
        state.resetForCellChange();
    }

    static void tabbyTimingAndQueue(Instance &instance, const std::string &program,
                                    bool wayland, int deletes, int overrideMs) {
        SKeyEngine engine(&instance, false);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), program);
        input.frontendName = "dbus";
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::Preedit);
        input.setCursorRect(Rect(700, 480, 700, 508));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        state.cachedIsChromium_ = 1;
        // Even an IDE with a shell child must retain its editor timing.
        state.cachedIsTerminalApp_ = 1;
        state.cachedIsFirefoxOrSnap_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        const auto appKey = skey::appDelayKey(program, wayland);
        engine.appDelayOverrides_[appKey].preCommitMs = overrideMs;
        state.lastActivateUsec_ = now(CLOCK_MONOTONIC);
        state.viet_.setRawInput("twj");
        state.committedLen_ = 1;
        state.uinputDeleting_ = true;
        state.expectedUinputBackspaces_ = state.seenUinputBackspaces_ = deletes;
        state.uinputBsOutstanding_ = 1;
        state.pendingUinputCommit_ = "ự";
        state.uinputPendingFinalLen_ = 2;
        state.bsRtEwma_ = 1000;
        state.bsSentAt_ = now(CLOCK_MONOTONIC) - 1000;
        KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(anchor);
        const auto delay = engine.appDelayStats_.at(appKey).lastSleepUsec;
        const uint64_t expected = overrideMs >= 0 ? overrideMs * 1000u :
            program == "tabby" ? deletes * 15000u : 50000u;
        check(anchor.accepted() && input.commits.empty() &&
                  (wayland ? delay < 15000 : delay == expected),
              "Tabby X11 retains per-delete headroom without browser/first-focus floor; other routes and override stay intact");
        if (!wayland && program == "tabby") {
            KeyEvent space(&input, Key(FcitxKey_space), false, 2000);
            KeyEvent n(&input, Key(FcitxKey_n), false, 2010);
            KeyEvent release(&input, Key(FcitxKey_n), true, 2011);
            state.keyEvent(space); state.keyEvent(n); state.keyEvent(release);
            state.finishUinputSettle(0, 0);
            check(input.commits == std::vector<std::string>{"ự"} && state.settledReplayTimer_,
                  "Tabby dispatches replacement before queued appends");
            state.settledReplayTimer_.reset(); state.replaySettledKeys();
            state.settledReplayTimer_.reset(); state.replaySettledKeys();
            state.settledReplayTimer_.reset(); state.replaySettledKeys();
            // Printable replay stays on the commit channel for composition
            // clients (replaySettledKeys, 2026-10-28): raw keys and commits
            // use different queues, so a forwarded space could overtake the
            // replacement.  Printable releases are not re-sent.
            check(input.commits == std::vector<std::string>({"ự", " ", "n"}) &&
                      input.forwarded.empty(),
                  "Tabby replays queued ASCII keys in order on the commit channel");
        }
        state.resetForCellChange();
        if (!wayland && program == "tabby" && deletes == 1 && overrideMs < 0) {
            state.viet_.setShortW(true);
            state.viet_.setRawInput("t");
            state.committedLen_ = 1;
            KeyEvent w(&input, Key(FcitxKey_w), false, 3000);
            state.keyEvent(w);
            check(w.accepted() && state.uinputSettling_,
                  "Tabby short-w append uses the commit dispatch barrier");
            state.finishUinputSettle(0, 0);
            check(state.settledReplayTimer_ && state.settledKeys_.empty(),
                  "Tabby protects a dispatched commit even before the next key arrives");
            KeyEvent j(&input, Key(FcitxKey_j), false, 3010);
            state.keyEvent(j);
            check(j.accepted() && state.settledKeys_.size() == 1 && !state.uinputDeleting_,
                  "a newly arriving tone cannot inject Backspace ahead of the append");
            state.resetForCellChange();
            check(!state.settledReplayTimer_ && state.settledKeys_.empty(),
                  "focus boundary cancels Tabby pending replay and its guard");
        }
    }

    static void chromiumX11RepeatedTone(Instance &instance, bool sheets) {
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(SKeyOutputMode::SurroundingText);
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
        // Fresh node without editor flags: must not imply fast rendering.
        engine.a11yMonitor_->focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
        engine.a11yMonitor_->sheetsEditorFocused_ = sheets;
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::Preedit);
        input.setCursorRect(Rect(718, 481, 718, 509));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome-stable";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.modeCacheValid_ = true;
        state.viet_.setRawInput("chafo");
        state.committedLen_ = 4;
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "isolate repeated tone transport");
        captureUinput(state, sockets[0]);
        for (char tone : {'s', 'j'}) {
            const std::string suffix = tone == 's' ? "áo" : "ạo";
            KeyEvent key(&input, Key(static_cast<KeySym>(tone)));
            state.keyEvent(key);
            uint32_t request[4] = {};
            check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request) &&
                      request[0] == 3 && state.pendingUinputCommit_ == suffix,
                  "each tone replaces exactly two characters and preserves ch");
            for (int i = 0; i < 2; ++i) {
                KeyEvent deletion(&input, Key(FcitxKey_BackSpace));
                state.keyEvent(deletion);
                check(!deletion.accepted(), "real suffix deletion reaches the editor");
            }
            state.bsRtEwma_ = 1000;
            state.bsSentAt_ = now(CLOCK_MONOTONIC) - 1000;
            KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
            state.keyEvent(anchor);
            const auto delay = engine.appDelayStats_.at(
                skey::appDelayKey("google-chrome-stable", false)).lastSleepUsec;
            check(anchor.accepted() && delay == (sheets ? 15000u : 30000u),
                  "fresh unknown X11 editor keeps 30ms floor; known Sheets keeps 15ms");
            // Inspect completion separately from the policy's timer duration.
            state.finishUinputSettle(0, 0);
            check(state.committedLen_ == 4 && state.viet_.getComposed() == "ch" + suffix,
                  "tone replacement preserves complete composed word and length");
        }
        check(input.commits == std::vector<std::string>{"áo", "ạo"},
              "repeated tones emit both replacement suffixes exactly once");
        state.resetForCellChange();
        state.uinputTestSend_ = {};
        close(sockets[1]);
    }

    static void chromiumX11SurroundingFallback(Instance &instance, int snapshot, char tone) {
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(SKeyOutputMode::SurroundingText);
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlags(CapabilityFlag::Preedit) |
            (snapshot ? CapabilityFlags(CapabilityFlag::SurroundingText) : CapabilityFlags()));
        input.setCursorRect(Rect(718, 481, 718, 509));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome-stable";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.modeCacheValid_ = true;
        state.viet_.setShortW(true);
        state.bsRtEwma_ = 1000;
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "isolate Chrome X11 surrounding fallback transport");
        captureUinput(state, sockets[0]);
        for (auto sym : {FcitxKey_t, FcitxKey_w}) {
            KeyEvent key(&input, Key(sym));
            state.keyEvent(key);
        }
        check(input.commits == std::vector<std::string>{"t", "ư"},
              "Surrounding Text commits the initial tw as tư");
        input.commits.clear();
        if (snapshot >= 2) {
            input.surroundingText().setText(snapshot == 3 ? "tư" : "zz", 2, 2);
            input.updateSurroundingText();
        }
        KeyEvent key(&input, Key(static_cast<KeySym>(tone)));
        state.keyEvent(key);
        const std::string suffix = tone == 'j' ? "ự" : "ừ";
        uint32_t request[4] = {};
        if (snapshot == 3) {
            check(input.deletions == 1 && input.commits == std::vector<std::string>{suffix} &&
                      recv(sockets[1], request, sizeof(request), MSG_DONTWAIT) < 0 && errno == EAGAIN,
                  "matching native surrounding retains native deletion without Uinput");
        } else {
            check(state.uinputDeleting_ && input.forwarded.empty() && input.commits.empty() &&
                      !state.hasDeferredCommitPending() &&
                      recv(sockets[1], request, sizeof(request), 0) == sizeof(request) && request[0] == 2,
                  "missing or stale Chrome X11 surrounding uses one deletion plus sync anchor");
            auto delayedDeletion = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + 25000, 1,
                [&](EventSourceTime *, uint64_t) {
                    check(input.commits.empty(), "no blind 15ms commit while deletion has not returned");
                    KeyEvent deletion(&input, Key(FcitxKey_BackSpace));
                    state.keyEvent(deletion);
                    check(!deletion.accepted(), "one real Backspace reaches the editor");
                    KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
                    state.keyEvent(anchor);
                    check(anchor.accepted() && state.uinputSettling_ && input.commits.empty(),
                          "sync anchor is consumed and renderer settle precedes replacement");
                    return false;
                });
            auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + 125000, 1,
                [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
            instance.eventLoop().exec();
            check(input.commits == std::vector<std::string>{suffix} &&
                      state.committedLen_ == 2 && !state.uinputDeleting_,
                  "replacement suffix commits once after deletion, preserving t");
        }
        state.resetForCellChange();
        state.uinputTestSend_ = {};
        close(sockets[1]);
    }

    static void blindA11yFieldReset(Instance &instance, bool uinput) {
        // With the AT-SPI focus snapshot stale and a word in flight, a
        // surrounding push that reports a DIFFERENT, non-empty text with
        // cursor == anchor == 0 must reset the composition so the next key
        // starts a new word instead of replacing text that is no longer at
        // the caret.  The settled state of the current field — an empty
        // field reporting "" at cursor 0, or the same text re-pushed after
        // a click at its start — must never trip the fallback.
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(uinput ? SKeyOutputMode::Uinput
                                                  : SKeyOutputMode::SurroundingText);
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();  // freshness 0 = blind
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome-stable";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = uinput ? SKeyOutputMode::Uinput
                                   : SKeyOutputMode::SurroundingText;
        state.modeCacheValid_ = true;

        auto seed = [&](const char *word) {
            state.viet_.reset();
            state.committedLen_ = 0;
            for (const char *p = word; *p; ++p) {
                // keyEvent() copies surrLastKeyUsec_ into surrPrevKeyUsec_ at
                // entry, so the quiet window must be seeded there.
                state.surrLastKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
                KeyEvent event(&input, Key(static_cast<KeySym>(*p)));
                state.keyEvent(event);
            }
        };
        auto push = [&](const std::string &text, unsigned cursor, unsigned anchor) {
            input.surroundingText().setText(text, cursor, anchor);
            input.updateSurroundingText();
        };
        auto key = [&](FcitxKeySym sym) {
            state.surrLastKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
            KeyEvent event(&input, Key(sym));
            state.keyEvent(event);
        };
        auto setBase = [&](const std::string &text) {
            state.lastKeySurrText_ = text;
            state.lastKeySurrValid_ = true;
        };

        // Typing a new word into an empty field: the field settles at ""
        // (cursor 0) and must not look like a field change.
        push("", 0, 0);
        seed("cha");
        check(state.viet_.getRawInput() == "cha",
              "typing into an empty field does not trip the blind fallback");

        // A late empty-field push after a real word (our own delete-to-empty
        // catching up) is likewise not a field change.
        push("chao", 4, 4);
        seed("cha");
        push("", 0, 0);
        key(FcitxKey_d);
        check(state.viet_.getRawInput() == "chad",
              "an empty-field push after a real word is not a field change");

        push("chao", 4, 4);
        seed("cha");
        push("nội dung ô mới", 0, 0);
        key(FcitxKey_m);
        check(state.viet_.getRawInput() == "m" && state.viet_.getComposed() == "m",
              "blind fallback resets the word when a new field text is reported");

        state.viet_.reset();
        push("cha", 3, 3);
        seed("cha");
        key(FcitxKey_n);
        check(state.viet_.getRawInput() == "chan",
              "the same text keeps the word in flight");

        // A push reproducing the composed word is our own commit's report
        // (the field holds only the word), not a field change.
        state.viet_.reset();
        for (char ch : std::string("gox"))
            state.viet_.processKey(ch);
        state.lastKeySurrText_ = "";
        state.lastKeySurrValid_ = true;
        state.surrPrevKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
        push("gõ", 0, 0);
        check(!state.checkBlindFieldChange() && state.viet_.getRawInput() == "gox",
              "a push reproducing the composed word is the same field");

        // FB-composer-style pushes: the app reports the whole text with
        // cursor 0, growing at the real caret — the snapshot grows at the
        // front and stays the same field.
        state.viet_.reset();
        state.committedLen_ = 0;
        for (char ch : std::string("t"))
            state.viet_.processKey(ch);
        setBase("\n");
        state.surrPrevKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
        push("t\n", 0, 0);
        check(!state.checkBlindFieldChange() && state.viet_.getRawInput() == "t",
              "a commit at the field start grows the text — same field");
        state.viet_.reset();
        for (char ch : std::string("th"))
            state.viet_.processKey(ch);
        push("tha\n", 0, 0);
        check(!state.checkBlindFieldChange() && state.viet_.getRawInput() == "th",
              "a push containing the composed word is the same field");

        // The mid-word replacement transient: the deletion removed a middle
        // character while both ends stay intact — same field.
        setBase("tha\n");
        push("th\n", 0, 0);
        check(!state.checkBlindFieldChange(),
              "a middle deletion keeps both ends — same field");
        setBase("thât\n");
        push("th\n", 0, 0);
        check(!state.checkBlindFieldChange(),
              "a longer middle deletion keeps both ends — same field");

        // The unresolved-program race (empty name at activation) is the
        // Chromium Wayland case too — the fallback must stay active.
        {
            TestInput unnamed(instance.inputContextManager(), "");
            unnamed.setFocusGroup(&group);
            auto &st = *unnamed.propertyFor(&engine.factory_);
            st.cachedProgram_ = "";
            st.cachedIsChromium_ = 0;
            st.cachedIsFirefoxOrSnap_ = st.cachedIsTerminalApp_ = 0;
            st.cachedMode_ = uinput ? SKeyOutputMode::Uinput
                                    : SKeyOutputMode::SurroundingText;
            st.modeCacheValid_ = true;
            for (char ch : std::string("cha"))
                st.viet_.processKey(ch);
            st.lastKeySurrText_ = "chao";
            st.lastKeySurrValid_ = true;
            st.surrPrevKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
            unnamed.surroundingText().setText("ô mới", 0, 0);
            unnamed.updateSurroundingText();
            check(st.viet_.getRawInput().empty() && st.lastKeySurrText_ == "ô mới",
                  "an unresolved program name still uses the blind fallback");
        }

        engine.a11yMonitor_->focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
        seed("cha");
        push("nội dung ô mới", 0, 0);
        key(FcitxKey_b);
        check(state.viet_.getRawInput() == "chab",
              "a fresh AT-SPI snapshot disables the blind fallback");
        engine.a11yMonitor_->focusSnapshotUsec_ = 0;

        // Our own deletions strip characters before the caret: a same-field
        // transient at cursor 0 (word at the field start) leaves the text
        // after the caret — a suffix of the snapshot — and must not look
        // like a field change.
        state.viet_.reset();
        push("cha", 3, 3);
        seed("cha");
        push("ha", 0, 0);
        check(state.viet_.getRawInput() == "cha" &&
                  !state.checkCellSelection(),
              "a suffix text at cursor 0 is our own deletion, not a field change");

        seed("cha");
        state.surrPrevKeyUsec_ = now(CLOCK_MONOTONIC);
        state.surrLastKeyUsec_ = now(CLOCK_MONOTONIC);
        push("ô khác nữa", 0, 0);
        {
            KeyEvent event(&input, Key(FcitxKey_c));
            state.keyEvent(event);
        }
        check(state.viet_.getRawInput() == "chac",
              "an unsettled surrounding push does not trigger the blind fallback");

        state.viet_.reset();
        state.committedLen_ = 0;
        state.surrLastKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
        state.uinputPendingFinalLen_ = 7;
        push("ô khác nữa 2", 0, 0);
        key(FcitxKey_m);
        check(state.uinputPendingFinalLen_ == 7 && state.viet_.getRawInput() == "m",
              "no word in flight leaves the field switch harmless");

        // A click into another field produces no push of its own: the
        // BS-forward of a stale-cache replacement lands in the new field and
        // ITS push is the first evidence.  The push fires the reset
        // immediately (surroundingTextChanged), cancels the deferred
        // replacement and replays the trigger key as a fresh first letter.
        if (!uinput) {
            state.viet_.reset();
            state.committedLen_ = 2;
            for (char ch : std::string("gox"))
                state.viet_.processKey(ch);
            check(state.viet_.getComposed() == "gõ", "seed completed word");
            setBase("go");
            push("go", 2, 2);  // the tone-replace push never arrived
            input.commits.clear();
            key(FcitxKey_g);
            push("ô mới", 0, 0);  // post-BS push of the newly focused field
            auto stop = instance.eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 60000, 1,
                [&](EventSourceTime *, uint64_t) {
                    instance.eventLoop().exit();
                    return true;
                });
            instance.eventLoop().exec();
            check(input.commits.empty() && state.viet_.getRawInput() == "g",
                  "stale replacement is dropped and the trigger key restarts the word");
        }

        // Uinput replacements run entirely under the delete machinery; the
        // new field's first push must still fire the reset mid-replacement
        // and re-deliver the trigger key.  A pair-transform in an empty
        // field deletes the WHOLE word, so its transient "" push must not
        // look like a field change.
        if (uinput) {
            state.viet_.reset();
            state.committedLen_ = 2;
            for (char ch : std::string("dd"))
                state.viet_.processKey(ch);
            check(state.viet_.getComposed() == "đ", "seed pair word");
            setBase("dd");
            push("dd", 2, 2);
            state.surrLastKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
            state.uinputDeleting_ = true;
            state.pendingUinputCommit_ = "đ";
            state.uinputPendingFinalLen_ = 1;
            push("", 0, 0);
            check(state.viet_.getRawInput() == "dd" &&
                      state.uinputDeleting_ && state.pendingUinputCommit_ == "đ",
                  "a pair-transform empty transient is not a field change");

            state.viet_.reset();
            state.committedLen_ = 4;
            for (char ch : std::string("goxg"))
                state.viet_.processKey(ch);
            setBase("go");
            push("go", 2, 2);
            state.surrLastKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
            state.uinputDeleting_ = true;
            state.pendingUinputCommit_ = "oxg";
            state.uinputPendingFinalLen_ = 4;
            push("", 0, 0);  // first push of the newly focused empty cell
            check(state.viet_.getRawInput() == "g" &&
                      state.pendingUinputCommit_.empty() &&
                      !state.uinputDeleting_,
                  "uinput machinery resets and re-delivers the trigger key on the new field push");

            // Blind-mode miss repair: a fresh unchanged snapshot proves the
            // first BS was consumed without deleting (autocomplete
            // dropdown) — the transaction restarts with one BS plus anchor
            // before committing.
            {
                state.viet_.reset();
                state.committedLen_ = 2;
                for (char ch : std::string("gox"))
                    state.viet_.processKey(ch);
                setBase("go");
                push("go", 2, 2);
                state.uinputDeletedWord_ = "go";
                state.pendingUinputCommit_ = "õ";
                state.uinputPendingFinalLen_ = 2;
                state.uinputDeleting_ = true;
                state.uinputSettling_ = true;
                state.uinputRepairCount_ = 0;
                state.uinputDeleteAck_.start("go", 2, 2, "go", 1, now(CLOCK_MONOTONIC));
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                push("go", 2, 2);  // unchanged snapshot after the echo window
                check(state.uinputDeleteAck_.missed(),
                      "an unchanged snapshot marks the deletion missed");
                int sockets[2];
                check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
                      "isolate blind repair transport");
                captureUinput(state, sockets[0]);
                state.finishUinputSettle(0, 0);  // ack deadline, then repair
                auto drive = instance.eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 120000, 1,
                    [&](EventSourceTime *, uint64_t) {
                        uint32_t request[4] = {};
                        check(recv(sockets[1], request, sizeof(request), MSG_DONTWAIT) == sizeof(request) &&
                                  request[0] == 2,
                              "a missed deletion restarts the transaction with one BS plus anchor");
                        input.commits.clear();
                        KeyEvent bs1(&input, Key(FcitxKey_BackSpace));
                        state.keyEvent(bs1);
                        KeyEvent bs2(&input, Key(FcitxKey_BackSpace));
                        state.keyEvent(bs2);
                        return false;
                    });
                auto stop = instance.eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 250000, 1,
                    [&](EventSourceTime *, uint64_t) {
                        check(input.commits == std::vector<std::string>{"õ"},
                              "the repair commits after the restarted deletion");
                        instance.eventLoop().exit();
                        return true;
                    });
                instance.eventLoop().exec();
                state.uinputTestSend_ = {};
                close(sockets[1]);
                state.uinputSettling_ = false;
                state.uinputDeleting_ = false;
                state.pendingUinputCommit_.clear();
            }

            // A late pre-BS echo can re-deliver the unchanged snapshot —
            // once the real post-delete state arrives, the miss must be
            // discarded and the commit proceed without a repair BS.
            {
                state.viet_.reset();
                state.committedLen_ = 2;
                for (char ch : std::string("gox"))
                    state.viet_.processKey(ch);
                setBase("go");
                push("go", 2, 2);
                state.uinputDeletedWord_ = "go";
                state.pendingUinputCommit_ = "õ";
                state.uinputPendingFinalLen_ = 2;
                state.uinputDeleting_ = true;
                state.uinputSettling_ = true;
                state.uinputRepairCount_ = 0;
                state.uinputDeleteAck_.start("go", 2, 2, "go", 1, now(CLOCK_MONOTONIC));
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                push("go", 2, 2);  // the echo arrives after the echo window
                check(state.uinputDeleteAck_.missed(),
                      "the echo marks a miss before any acknowledgement");
                push("g", 1, 1);   // the real post-delete state arrives
                std::this_thread::sleep_for(std::chrono::milliseconds(10));  // past the ack guard
                int sockets[2];
                check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
                      "isolate echo-miss transport");
                captureUinput(state, sockets[0]);
                input.commits.clear();
                state.finishUinputSettle(0, 0);  // synchronous — the ack guard is past
                uint32_t request[4] = {};
                check(recv(sockets[1], request, sizeof(request), MSG_DONTWAIT) < 0 &&
                          errno == EAGAIN,
                      "an echo miss commits without a repair BS");
                check(input.commits == std::vector<std::string>{"õ"},
                      "the echo-miss path still commits");
                state.uinputTestSend_ = {};
                close(sockets[1]);
                state.uinputSettling_ = false;
                state.uinputDeleting_ = false;
                state.pendingUinputCommit_.clear();
            }

            // Autofill selection: the caret/anchor enclose the suggestion,
            // and the dropdown dismissal can leave the word's tail intact
            // ("google.com" selected → "go").  A fresh snapshot still
            // containing the word must trigger the same repair.
            {
                state.viet_.reset();
                state.committedLen_ = 2;
                for (char ch : std::string("gox"))
                    state.viet_.processKey(ch);
                setBase("google.com");
                push("google.com", 2, 10);
                state.uinputDeletedWord_ = "go";
                state.pendingUinputCommit_ = "õ";
                state.uinputPendingFinalLen_ = 2;
                state.uinputDeleting_ = true;
                state.uinputSettling_ = true;
                state.uinputRepairCount_ = 0;
                state.uinputDeleteAck_.start("google.com", 2, 10, "go", 1,
                                             now(CLOCK_MONOTONIC) - 100000);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                push("go", 2, 2);  // the suggestion vanished, the tail intact
                check(state.uinputDeleteAck_.missed(),
                      "a fresh snapshot still containing the word marks a miss");
                int sockets[2];
                check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
                      "isolate selection-miss transport");
                captureUinput(state, sockets[0]);
                state.finishUinputSettle(0, 0);  // synchronous — the deadline is past
                uint32_t request[4] = {};
                check(recv(sockets[1], request, sizeof(request), MSG_DONTWAIT) == sizeof(request) &&
                          request[0] == 2,
                      "a selection-consumed deletion restarts with one BS plus anchor");
                state.uinputTestSend_ = {};
                close(sockets[1]);
                state.uinputSettling_ = false;
                state.uinputDeleting_ = false;
                state.pendingUinputCommit_.clear();
            }

            // Post-commit verification: the app's push of the result ending
            // with the intact word plus the committed text proves the
            // deletion failed silently — restart the transaction.
            {
                state.viet_.reset();
                state.committedLen_ = 2;
                for (char ch : std::string("gox"))
                    state.viet_.processKey(ch);
                setBase("go");
                state.surrPrevKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
                state.uinputDeletedWord_ = "go";
                state.uinputDeletedLen_ = 1;
                state.uinputPostVerify_ = true;
                state.uinputCommittedText_ = "õ";
                state.uinputPostFinalLen_ = 2;
                state.uinputPostVerifyDeadlineUsec_ = now(CLOCK_MONOTONIC) + 600000;
                int sockets[2];
                check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
                      "isolate post-verify transport");
                captureUinput(state, sockets[0]);
                push("goõ", 2, 2);  // the silent-dropdown result
                uint32_t request[4] = {};
                check(recv(sockets[1], request, sizeof(request), MSG_DONTWAIT) == sizeof(request) &&
                          request[0] == 3 &&
                          state.pendingUinputCommit_ == "õ",
                      "a silent deletion failure repairs after the committed push");
                state.uinputTestSend_ = {};
                close(sockets[1]);
                state.uinputPostVerify_ = false;
                state.uinputDeleting_ = false;
                state.pendingUinputCommit_.clear();
                state.expectedUinputBackspaces_ = 0;
                state.seenUinputBackspaces_ = 0;
            }
        }

        state.resetForCellChange();
    }

    static void chromiumCompositionCommit(Instance &instance, bool wayland,
                                          bool preeditCap, bool url,
                                          const char *program, bool replacement, int sheets = 0,
                                          bool omnibox = false, const std::string &suffix = "ự") {
        // Model an editor requiring composition for replacement input, not
        // Facebook itself. Multi-character suffixes must use that path too.
        class CompositionInput : public TestInput {
        public:
            CompositionInput(InputContextManager &manager, const char *program)
                : TestInput(manager, program) {}
            // The composition policy (input_field_policy.h) is keyed on the
            // frontend: an X11 embedded Chromium client (Tabby) talks D-Bus.
            const char *frontend() const override { return "dbus"; }
            std::string composition, visible;
            std::vector<std::string> events;
        protected:
            void updatePreeditImpl() override {
                composition = inputPanel().clientPreedit().toString();
                events.push_back("preedit:" + composition);
            }
            void commitStringImpl(const std::string &text) override {
                events.push_back("commit:" + text);
                // Adversarial editor ignores bare replacement commits.
                if (!composition.empty()) visible += text;
                composition.clear();
                TestInput::commitStringImpl(text);
            }
        };
        SKeyEngine engine(&instance, false);
        engine.config_.showPreedit.setValue(false);
        if (sheets) {
            engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
            engine.a11yMonitor_->sheetsEditorFocused_ = sheets != 3;
            engine.a11yMonitor_->googleDocsDocFocused_ = sheets == 3;
            engine.a11yMonitor_->focusSnapshotUsec_ =
                now(CLOCK_MONOTONIC) - (sheets == 2 ? 6000000 : 0);
        }
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        CompositionInput input(instance.inputContextManager(), program);
        input.setFocusGroup(&group);
        if (omnibox) input.setCursorRect(Rect(200, 54, 201, 72));
        input.setCapabilityFlags((preeditCap ? CapabilityFlags(CapabilityFlag::Preedit) : CapabilityFlags()) |
                                 (url ? CapabilityFlags(CapabilityFlag::Url) : CapabilityFlags()));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        const bool firefox = std::string(program) == "firefox";
        state.cachedIsChromium_ = firefox ? 0 : 1;
        state.cachedIsFirefoxOrSnap_ = firefox ? 1 : 0;
        state.cachedIsTerminalApp_ = 0;
        // Native X11 embedded Chromium clients (Tabby, VS Code) compose every
        // commit — input_field_policy.h usesCompositionCommits(), keyed on
        // the runtime and frontend, not on Url or suffix length (2026-10-28).
        const std::string name(program);
        const bool compositionClient = !wayland && preeditCap && sheets != 1 &&
                                       (name == "tabby" || name == "code");
        const bool expected = compositionClient ||
            (preeditCap && !url && !omnibox && sheets != 1 &&
             replacement && (name == "google-chrome" ||
                             (!wayland && firefox && sheets != 3)) &&
             (name == "google-chrome" || utf8::length(suffix) == 1));
        check(state.commitText(suffix, replacement), "replacement commit issued");
        check(input.commits == std::vector<std::string>{suffix}, "single suffix commit without retries");
        if (input.visible != (expected ? suffix : ""))
            std::cerr << "composition case: program=" << program << " wayland=" << wayland
                      << " preedit=" << preeditCap << " url=" << url << " sheets=" << sheets
                      << " omnibox=" << omnibox << " suffix=" << suffix << '\n';
        check(input.visible == (expected ? suffix : ""), "composition selects text insertion instead of character key route");
        check(input.events == (expected ? std::vector<std::string>{"preedit:" + suffix, "commit:" + suffix, "preedit:"}
                                        : std::vector<std::string>{"commit:" + suffix}),
              "composition must precede commit and be cleared afterwards only in scoped route");
        check(input.inputPanel().clientPreedit().empty(), "no lingering composition even when Show preedit is off");
    }

    static void chromiumDeletionAck(Instance &instance, int ackDelayMs, bool suffixPath = false, bool firefox = false, char tone = 'j', bool chromiumX11 = false) {
        class TimedInput : public TestInput {
        public:
            TimedInput(InputContextManager &manager, const char *program) : TestInput(manager, program) {}
            uint64_t committedAt = 0;
            bool replacementHadComposition = false;
        protected:
            void commitStringImpl(const std::string &text) override {
                if (text == "ự" || text == "ừ") replacementHadComposition =
                    inputPanel().clientPreedit().toString() == text;
                committedAt = now(CLOCK_MONOTONIC);
                TestInput::commitStringImpl(text);
            }
        };
        const char *program = firefox ? "firefox" : "google-chrome";
        const std::string suffix = tone == 'j' ? "ự" : "ừ";
        const std::string word = "t" + suffix;
        SKeyEngine engine(&instance, false);
        engine.config_.autoDelay.setValue(true);
        // Exercise the regular 80ms acknowledgement deadline. Blind-mode
        // repair has its own shorter deadline and separate regression cases.
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
        engine.a11yMonitor_->focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
        const bool wayland = !firefox && !chromiumX11;
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TimedInput input(instance.inputContextManager(), program);
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlags(CapabilityFlag::SurroundingText) | CapabilityFlag::Preedit);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        state.cachedIsChromium_ = firefox ? 0 : 1;
        state.cachedIsFirefoxOrSnap_ = firefox ? 1 : 0;
        state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.viet_.setShortW(true);
        state.bsRtEwma_ = 1000;
        engine.noteAppRoundTrip(program, wayland, 1000);
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "create isolated Chrome deletion transport");
        captureUinput(state, sockets[0]);
        KeyEvent t(&input, Key(FcitxKey_t));
        state.keyEvent(t);
        check(!t.accepted(), "initial Chrome Uinput letter is forwarded");
        input.surroundingText().setText("xin t!", 5, 5);
        input.updateSurroundingText();
        KeyEvent w(&input, Key(FcitxKey_w));
        state.keyEvent(w);
        check(w.accepted() && input.commits.size() == 1 && input.commits[0] == "ư",
              "short-w appends ư before the tone replacement");
        if (firefox) {
            // App has not published the ư append yet. GTK resets now.
            input.surroundingText().setText("xin t!", 5, 5);
            input.updateSurroundingText();
            state.reset();
            check(!state.surrResetTentative_ && state.viet_.getComposed() == "tư",
                  "reset after short-w commit must preserve composition despite lagging surrounding");
        }
        input.commits.clear();
        input.committedAt = 0;
        input.surroundingText().setText("xin tư!", 6, 6);
        input.updateSurroundingText();
        if (suffixPath) {
            state.viet_.setRawInput(word);
            state.surroundingCommit("tư", word); // buffered replay's replacement path
        } else {
            KeyEvent j(&input, Key(static_cast<KeySym>(tone)));
            state.keyEvent(j);
            check(j.accepted() && state.viet_.getComposed() == word,
                  "twj must produce tự through the normal Uinput key path");
        }
        if (firefox) {
            state.uinputKeyForwarded_ = false; // delayed reset from the w commit
            state.reset();
            check(!state.surrResetTentative_ && state.viet_.getComposed() == word &&
                      state.uinputDeleting_ && state.pendingUinputCommit_ == suffix,
                  "Firefox reset before first BS must preserve transaction without tentative reattach");
        }
        check(state.uinputDeleteAck_.active(), "matching original word must arm real deletion observation");
        const auto sentAt = state.bsSentAt_;
        uint32_t request[4] = {};
        check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request) && request[0] == 2,
              "one deletion and a sync anchor must be sent");
        KeyEvent deletion(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(deletion);
        check(!deletion.accepted(), "real deletion passes to Chrome");
        if (firefox) {
            state.reset();
            check(state.viet_.getComposed() == word && !state.surrResetTentative_,
                  "Firefox reset between deletion and anchor keeps composed word");
        }
        KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(anchor);
        check(anchor.accepted() && state.uinputSettling_, "sync anchor schedules observed settle");
        const auto key = skey::appDelayKey(program, wayland);
        check(engine.appDelayStats_.at(key).lastSleepUsec ==
                  skey::UinputDeleteAck::guardUsec,
              "observed path starts at 8ms, not the unconditional 30ms floor");
        KeyEvent navigation(&input, Key(FcitxKey_End));
        state.keyEvent(navigation);
        check(navigation.accepted() && input.forwarded.empty(), "subsequent keys wait behind observed deletion");
        if (!wayland) {
            check(state.uinputCommitTimer_->time() == sentAt + skey::UinputDeleteAck::timeoutUsec,
                  "X11 waits for surrounding notification or one timeout instead of 4ms polls");
        }
        uint64_t acknowledgedAt = 0;
        auto update = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
            sentAt + static_cast<uint64_t>(ackDelayMs >= 0 ? ackDelayMs : 10) * 1000, 1,
            [&](EventSourceTime *, uint64_t) {
                if (ackDelayMs == -3) {
                    state.resetForCellChange();
                } else if (ackDelayMs != -1) {
                    input.surroundingText().setText(ackDelayMs == -2 ? "xin x!" : "xin t!", 5, 5);
                    acknowledgedAt = now(CLOCK_MONOTONIC);
                    input.updateSurroundingText();
                    if (!wayland && ackDelayMs >= 0) {
                        check(state.uinputCommitTimer_->time() >= acknowledgedAt + skey::UinputDeleteAck::guardUsec &&
                                  state.uinputCommitTimer_->time() < acknowledgedAt + skey::UinputDeleteAck::guardUsec + 2000,
                              "exact X11 acknowledgement rearms timer at the full guard deadline");
                    }
                }
                return true;
            });
        auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, sentAt + 120000, 1,
            [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
        instance.eventLoop().exec();
        if (ackDelayMs == -3) {
            check(input.commits.empty() && input.forwarded.empty() && !state.uinputDeleteAck_.active(),
                  "cell/focus-boundary cancellation must discard pending commit and queued navigation");
        } else {
            check(input.commits.size() == 1 && input.commits[0] == suffix && input.deletions == 0 &&
                      input.forwarded.size() == 1 && input.forwarded[0].key.sym() == FcitxKey_End,
                  "replacement and queued navigation complete once without speculative re-delete/re-commit");
            check(input.replacementHadComposition && input.inputPanel().clientPreedit().empty(),
                  "actual twj replacement must use composition and leave it cleared");
            const auto earliest = ackDelayMs >= 0 ? acknowledgedAt + skey::UinputDeleteAck::guardUsec
                                                 : sentAt + skey::UinputDeleteAck::timeoutUsec;
            check(input.committedAt >= earliest, "never commit before acknowledgement guard or bounded timeout");
            check(state.uinputAckUnavailable_ == (ackDelayMs < 0 && !wayland),
                  "X11 backs off missing acknowledgements; Wayland keeps family verification armed");
            if (ackDelayMs < 0) {
                input.surroundingText().setText("xin tư!", 6, 6);
                input.updateSurroundingText();
                state.sendBackspaceUinput(2, 0, "tư");
                check(state.uinputDeleteAck_.active() == wayland,
                      "missing acknowledgements back off on X11 and remain armed for Wayland browsers");
            }
        }
        state.uinputTestSend_ = {};
        close(sockets[1]);
    }

    static void chromiumAckScope(Instance &instance, bool wayland,
                                 const char *program, bool autoDelay,
                                 int overrideMs, bool url, bool expected,
                                 bool sheets = false, bool omnibox = false) {
        SKeyEngine engine(&instance, false);
        engine.config_.autoDelay.setValue(autoDelay);
        // Test the normal scope independently of the blind-browser repair
        // policy, which deliberately arms acknowledgement without a11y.
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
        engine.a11yMonitor_->sheetsEditorFocused_ = sheets;
        engine.a11yMonitor_->focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), program);
        input.setFocusGroup(&group);
        if (omnibox) input.setCursorRect(Rect(200, 54, 201, 72));
        input.setCapabilityFlags(CapabilityFlags(CapabilityFlag::SurroundingText) |
                                 (url ? CapabilityFlags(CapabilityFlag::Url) : CapabilityFlags()));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        engine.appDelayOverrides_[skey::appDelayKey(program, wayland)].preCommitMs = overrideMs;
        input.surroundingText().setText("tư", 2, 2);
        input.updateSurroundingText();
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "create scope-test transport");
        captureUinput(state, sockets[0]);
        state.sendBackspaceUinput(2, 0, "tư");
        if (state.uinputDeleteAck_.active() != expected)
            std::cerr << "Ack scope: program=" << program << " wayland=" << wayland
                      << " autoDelay=" << autoDelay << " overrideMs=" << overrideMs
                      << " url=" << url << " sheets=" << sheets << " omnibox=" << omnibox
                      << " expected=" << expected << " actual=" << state.uinputDeleteAck_.active() << '\n';
        check(state.uinputDeleteAck_.active() == expected,
              "ack timing respects X11 browser-page scope and Wayland family verification");
        if (expected) {
            engine.appDelayOverrides_[skey::appDelayKey(program, wayland)].preCommitMs = 7;
            state.uinputDeleting_ = true;
            state.expectedUinputBackspaces_ = state.seenUinputBackspaces_ = 1;
            state.pendingUinputCommit_ = "ự";
            KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
            state.handlePendingUinputBackspace(anchor);
            check(!state.uinputDeleteAck_.active(),
                  "manual override enabled mid-transaction cancels acknowledgement waiting");
            state.resetForCellChange();
        }
        state.uinputTestSend_ = {};
        close(sockets[1]);
    }

    static void chromiumRendererSettle(Instance &instance, int snapshot, int overrideMs = -1) {
        SKeyEngine engine(&instance, false);
        if (snapshot != 0) {
            engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
            engine.a11yMonitor_->focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
            engine.a11yMonitor_->focusFbCommentSig_ = snapshot == 1;
        }
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        const auto appKey = skey::appDelayKey("google-chrome", true);
        engine.appDelayOverrides_[appKey].preCommitMs = overrideMs;
        // Match the reported replacement's post-BS state. A surrounding
        // cursor of 1 must not be taken as proof that the renderer is ready
        // to accept the replacement. No real Uinput device is involved.
        input.surroundingText().setText("t", 1, 1);
        input.updateSurroundingText();
        state.viet_.setRawInput("tự");
        state.committedLen_ = 1;
        state.uinputDeleting_ = true;
        state.expectedUinputBackspaces_ = state.seenUinputBackspaces_ = 1;
        state.uinputBsOutstanding_ = 1;
        state.pendingUinputCommit_ = "ự";
        state.uinputPendingFinalLen_ = 2;
        state.bsRtEwma_ = 1000; // warm AutoDelay, like the 578-sample log
        state.bsSentAt_ = now(CLOCK_MONOTONIC) - 1000;
        const auto anchorAt = now(CLOCK_MONOTONIC);
        KeyEvent anchor(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(anchor);
        const auto delay = engine.appDelayStats_.at(appKey).lastSleepUsec;
        const uint64_t floor = snapshot == 2 ? 20000 : 30000;
        check(anchor.accepted() && state.uinputSettling_ && input.commits.empty(),
              "Chrome anchor must schedule replacement, not commit inline");
        check(overrideMs >= 0 ? delay == static_cast<uint64_t>(overrideMs) * 1000 : delay >= floor,
              "Chrome Wayland must retain renderer headroom despite fast learned loopbacks; explicit override wins");
        auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, anchorAt + delay + 30000, 1,
            [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
        instance.eventLoop().exec();
        check(input.commits.size() == 1 && input.commits[0] == "ự" &&
                  !state.uinputDeleting_ && !state.uinputSettling_ && state.committedLen_ == 2,
              "Chrome settle must emit the replacement once and complete the transaction");
    }

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

    static void addressBarSuffixReplacement(Instance &instance, bool staleCoordinates = false) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::Preedit);
        input.setCursorRect(Rect(200, 54, 201, 72));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome-stable";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "isolate address-bar replacement transport");
        captureUinput(state, sockets[0]);
        state.addrBarHadFirstWord_ = state.addrBarHadSpace_ = true;
        state.viet_.setRawInput("nhaats");
        state.scheduleAddrBarReplacement(2, "ất", 4, FcitxKey_s, 1000,
                                         "nhất", false, "nhât");
        check(state.expectedUinputBackspaces_ == 2 &&
                  state.pendingUinputCommit_ == "ất" && state.uinputPendingFinalLen_ == 4,
              "missing a11y must not rewrite unchanged prefix of subsequent word");
        uint32_t request[4];
        check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request) &&
                  request[0] == 3 && request[1] == 0,
              "subsequent word sends exact suffix plus anchor, without Escape");
        state.resetForCellChange();
        state.addrBarHadFirstWord_ = state.addrBarHadSpace_ = false;
        state.addrBarClearedByCtrlKey_ = true;
        state.viet_.setRawInput("banj");
        if (staleCoordinates) {
            engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
            auto &cache = engine.a11yMonitor_->textCache_;
            const auto stamp = now(CLOCK_MONOTONIC);
            cache.setEnabled(true, stamp);
            cache.focus(":1.2", "/omnibox", stamp);
            const auto request = cache.beginPoll(stamp);
            check(request.has_value(), "prepare pre-word omnibox snapshot");
            cache.finishPoll(*request, true, "http://127.0.0.1/", 16, 16, stamp);
            // The text snapshot is only trusted with a live focus snapshot
            // of this app (a11yFocusRelevant, 2026-10-28) — a stale one from
            // another Chrome window must not steer the replacement.
            engine.a11yMonitor_->browserUIFocused_ = true;
            engine.a11yMonitor_->focusSnapshotUsec_ = stamp;
        }
        state.scheduleAddrBarReplacement(2, "ạn", 3, FcitxKey_j, 1100,
                                         "bạn", true, "ban");
        check(state.expectedUinputBackspaces_ == (staleCoordinates ? 2 : 4) &&
                  state.pendingUinputCommit_ == (staleCoordinates ? "ạn" : "bạn") &&
                  recv(sockets[1], request, sizeof(request), 0) == sizeof(request) &&
                  request[0] == (staleCoordinates ? 3u : 5u) &&
                  request[1] == (staleCoordinates ? 1u : 0u),
              "unknown first-word autocomplete needs full replacement; a URL snapshot keeps the prefix safe");
        if (!staleCoordinates) {
            // Chrome 154 X11 can keep inline autocomplete selected after
            // Escape. The first real BS removes that selection, not 'n'.
            // Also exercise an absent selection: extra BS at bar start is
            // harmless, unlike an extra BS in a URL suffix.
            for (bool selected : {false, true}) {
                std::string visible = "ban";
                for (unsigned i = 0; i + 1 < request[0]; ++i) {
                    if (selected) selected = false;
                    else if (!visible.empty()) visible.pop_back();
                }
                visible += state.pendingUinputCommit_;
                check(visible == "bạn", "first-word replacement must consume inline autocomplete before deleting typed text");
            }
        }
        state.resetForCellChange();
        state.uinputTestSend_ = {};
        close(sockets[1]);
    }

    static void addressBarSelectAnchor(Instance &instance) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::Preedit);
        input.setCursorRect(Rect(194, 53, 195, 73));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome-stable";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        state.viet_.setRawInput("chaf");
        state.committedLen_ = state.uinputPendingFinalLen_ = 3;
        state.pendingUinputCommit_ = "à";
        state.uinputDeleting_ = state.uinputShiftSelect_ = true;
        state.expectedUinputBackspaces_ = 1;
        state.armAddrBarCycle();
        KeyEvent next(&input, Key(FcitxKey_o), false, 1200);
        state.keyEvent(next);
        check(next.accepted() && state.settledKeys_.size() == 1,
              "physical keys wait for the Select replacement");
        auto pass = [&](Key key, bool release) {
            KeyEvent event(&input, key, release);
            state.keyEvent(event);
            check(!event.accepted() && state.settledKeys_.size() == 1,
                  "Select modifier and selection events must bypass the X11 replay queue");
        };
        pass(Key(FcitxKey_Shift_L), false);
        pass(Key(FcitxKey_Left, KeyState::Shift), false);
        check(state.seenUinputBackspaces_ == 1,
              "Shift+Left must reach the selection counter before timeout");
        pass(Key(FcitxKey_Left, KeyState::Shift), true);
        pass(Key(FcitxKey_Shift_L), true);
        KeyEvent anchor(&input, Key(FcitxKey_Left));
        state.keyEvent(anchor);
        check(anchor.accepted() && state.uinputSettling_ &&
                  state.expectedUinputBackspaces_ == 0 && state.settledKeys_.size() == 1,
              "Select anchor is consumed and starts commit settling without entering the replay queue");
        state.finishUinputSettle(0, 0);
        check(input.commits == std::vector<std::string>{"à"},
              "Select commits the replacement once after its anchor");
        state.settledReplayTimer_.reset();
        state.replaySettledKeys();
        check(input.commits == std::vector<std::string>{"à", "o"} &&
                  state.viet_.getComposed() == "chào",
              "the queued o follows the replacement to form chào");
    }

    static void addressBarSettleAcrossFocus(Instance &instance) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::Preedit);
        input.setCursorRect(Rect(200, 54, 201, 72));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome-stable";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        state.viet_.setRawInput("nhaas");
        state.committedLen_ = 2;
        state.uinputPendingFinalLen_ = 3;
        state.pendingUinputCommit_ = "ấ";
        state.uinputDeleting_ = true;
        state.armAddrBarCycle();
        state.expectedUinputBackspaces_ = 1;
        KeyEvent escape(&input, Key(FcitxKey_Escape));
        state.keyEvent(escape);
        check(!escape.accepted() && state.settledKeys_.empty(),
              "autocomplete Escape must reach Chrome before deletion");
        KeyEvent t(&input, Key(FcitxKey_t), false, 1200);
        state.keyEvent(t);
        check(state.settledKeys_.size() == 1 && state.bufferedUinputKeys_.empty(),
              "pre-anchor omnibox keys use the same ordered queue as settle keys");
        state.expectedUinputBackspaces_ = 0;
        state.scheduleUinputSettle(12000, 0, 0);
        state.deactivate();
        check(input.commits.empty() && input.forwarded.empty() && state.uinputSettling_ &&
                  state.pendingUinputCommit_ == "ấ" && state.settledKeys_.size() == 1,
              "focus churn must not bypass settle delay or replay next letter while deactivating");
        state.activate();
        state.finishUinputSettle(0, 0);
        check(input.commits == std::vector<std::string>{"ấ"} && state.settledReplayTimer_,
              "replacement dispatches before the first queued key is replayed");
        state.settledReplayTimer_.reset();
        state.replaySettledKeys();
        check(input.commits == std::vector<std::string>{"ấ", "t"} &&
                  state.viet_.getComposed() == "nhất" && state.committedLen_ == 4 &&
                  input.forwarded.empty(),
              "reactivated settle restores final length before replaying t exactly once");
        state.settledKeys_.push_back({Key(FcitxKey_space), false, 1210});
        state.settledKeys_.push_back({Key(FcitxKey_n), false, 1220});
        state.settledKeys_.push_back({Key(FcitxKey_n), true, 1221});
        state.replaySettledKeys();
        check(state.settledReplayTimer_ && input.commits.back() == " ",
              "replay yields to the frontend between queued commits");
        state.settledReplayTimer_.reset();
        state.replaySettledKeys();
        state.settledReplayTimer_.reset();
        state.replaySettledKeys();
        check(input.commits == std::vector<std::string>{"ấ", "t", " ", "n"} &&
                  input.forwarded.empty() && state.viet_.getComposed() == "n",
              "queued printable letters and spaces use one ordered commit channel");
    }

    static void chromeFastRepeat(Instance &instance, bool pending, bool replay,
                                 const std::string &program = "google-chrome-stable") {
        SKeyEngine engine(&instance, false);
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), program);
        input.setFocusGroup(&group);
        input.setCursorRect(Rect(200, 54, 201, 72));
        if (program == "tabby") {
            // Tabby is a D-Bus composition client (preedit-capable): its
            // ordered queue comes from that policy, not from an omnibox.
            input.frontendName = "dbus";
            input.setCapabilityFlags(CapabilityFlag::Preedit);
        }
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        state.addrBarLastTriggerKey_ = FcitxKey_n;
        state.addrBarTriggerKeyTime_ = 1000;
        state.addrBarGuardArmedUsec_ = now(CLOCK_MONOTONIC);
        state.addrBarTriggerDeadline_ = now(CLOCK_MONOTONIC) + 100000;
        state.uinputDeleting_ = pending;
        state.expectedUinputBackspaces_ = pending ? 1 : 0;
        KeyEvent event(&input, Key(FcitxKey_n), false, replay ? 1000 : 1010);
        state.keyEvent(event);
        if (pending) {
            check(event.accepted() && state.settledKeys_.size() == 1 &&
                      state.bufferedUinputKeys_.empty(),
                  "in-flight omnibox keys retain their timestamp in the ordered queue");
            state.uinputDeleting_ = false;
            state.expectedUinputBackspaces_ = 0;
            state.replaySettledKeys();
            check(state.viet_.getComposed() == (replay ? "" : "n"),
                  "ordered replay drops identical timestamps but processes fresh repeats");
        } else {
            check(event.accepted() == replay && state.viet_.getComposed() == (replay ? "" : "n"),
                  "X11 browser must process a new timestamp within the 100ms guard");
        }
        state.resetForCellChange();
    }

    static void addressBarCycleExpiry(Instance &instance, bool rapid = false) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::Preedit);
        input.setCursorRect(Rect(239, 54, 240, 72));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome-stable";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        state.viet_.setRawInput("nhaast");
        state.committedLen_ = 4;
        state.armAddrBarCycle();
        state.reset();
        check(state.viet_.getRawInput() == "nhaast", "own-output reset inside guard preserves word");
        const auto deadline = state.addrBarCycleDeadline_;
        state.activate();
        check(state.addrBarCycleDeadline_ == deadline, "reactivation must not renew the output guard");
        if (!rapid) {
        state.addrBarCycleDeadline_ = now(CLOCK_MONOTONIC) - 1;
        state.uinputDeleting_ = true;
        state.pendingUinputCommit_ = "ấ";
        state.expireAddrBarCycle();
        check(state.addrBarExpectCycle_, "in-flight replacement retains cycle protection");
        state.uinputDeleting_ = false;
        state.pendingUinputCommit_.clear();
        // The real click first reports the old omnibox geometry, as in the log.
        state.reset();
        check(!state.addrBarExpectCycle_ && state.viet_.getRawInput().empty(),
              "later real reset must discard the address-bar word even with stale geometry");
        } else {
            state.reset();
            check(state.viet_.getRawInput() == "nhaast", "rapid focus churn initially preserves word");
        }
        state.deactivate();
        state.activate();
        input.setCursorRect(Rect(400, 400, 500, 425));
        state.addrBarUiVerdictAtUsec_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        for (char ch : std::string("ngon")) {
            KeyEvent event(&input, Key(static_cast<KeySym>(ch)));
            state.keyEvent(event);
            check(!event.accepted(), "new input letters must pass through without replacing old omnibox text");
        }
        check(state.viet_.getComposed() == "ngon" && input.commits.empty() &&
                  !state.uinputDeleting_ && state.lastRawInput_ != "nhaast",
              "new input must contain only ngon and no stale replacement or reclaim");
    }

    static void firefoxSelectAllBackspace(Instance &instance, int snapshot) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "firefox");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "firefox";
        state.cachedIsChromium_ = state.cachedIsTerminalApp_ = 0;
        state.cachedIsFirefoxOrSnap_ = 1;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.modeCacheValid_ = true;
        state.viet_.setRawInput("twj");
        state.viet_.setShortW(true);
        state.committedLen_ = 2;
        // Application selection is authoritative, independently of the IM cache.
        std::string visible = "tự từ";
        input.surroundingText().setText(visible, 5, 5);
        input.updateSurroundingText();
        KeyEvent selectAll(&input, Key(FcitxKey_a, KeyState::Ctrl));
        state.keyEvent(selectAll);
        check(!selectAll.accepted() && state.viet_.getRawInput().empty(),
              "Ctrl+A passes to Firefox and ends tracked composition");
        state.reset(); // GTK reset must not require a Ctrl+A latch to survive
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.modeCacheValid_ = true;
        if (snapshot == 0) input.surroundingText().invalidate();
        else input.surroundingText().setText(visible, snapshot == 3 ? 0 : 5,
                                           snapshot == 2 ? 0 : 5);
        input.updateSurroundingText();
        const auto commits = input.commits.size();
        KeyEvent backspace(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(backspace);
        check(!backspace.accepted() && input.deletions == 0 && input.forwarded.empty() &&
                  input.commits.size() == commits,
              "idle Firefox BS must pass original key exactly once even with stale collapsed selection");
        // The application's Ctrl+A selection receives the original key.
        if (!backspace.accepted()) visible.clear();
        check(visible.empty() && state.lastRawInput_.empty() && !state.reclaimReady_,
              "selection deletion must leave no stale word to reclaim");
        KeyEvent repeat(&input, Key(FcitxKey_BackSpace));
        state.keyEvent(repeat);
        check(!repeat.accepted() && input.deletions == 0,
              "repeated BS must not issue deletion against the stale pre-selection cache");
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
        captureUinput(state, sockets[0]);
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
        state.uinputTestSend_ = {};
        close(sockets[1]);
    }

    static void firefoxFallback(Instance &instance, int snapshot, bool docs) {
        SKeyEngine engine(&instance, false);
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
        engine.a11yMonitor_->googleDocsDocFocused_ = docs;
        // The Docs verdict needs a live focus snapshot of this app
        // (a11yGoogleDocsFocused → a11yFocusRelevant).
        if (docs) engine.a11yMonitor_->focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
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
        captureUinput(state, sockets[0]);
        state.surroundingCommit("go", "gõ");
        uint32_t request[4] = {};
        check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request) && request[0] == 2,
              "Firefox missing/stale surrounding or Docs must retain Uinput fallback");
        check(input.deletions == 0 && input.commits.empty() && state.uinputDeleting_,
              "fallback must wait for Backspace before committing");
        state.resetForCellChange();
        state.uinputTestSend_ = {};
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
        // Capture engine requests without opening a real uinput device or
        // injecting any keys into the user's applications.
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "create isolated uinput transport");
        captureUinput(state, sockets[0]);
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
            int steps = 0;
            while (state.uinputDeleting_ || state.settledReplayTimer_) {
                check(++steps <= 20, "Office replay must terminate");
                if (state.settledReplayTimer_) {
                    // Model the dispatch barrier as well as the settle timer.
                    input.flushCommits();
                    state.settledReplayTimer_.reset();
                    state.replayBufferedUinputKeys();
                    continue;
                }
                check(++replacements <= 4, "replacement replay must terminate");
                uint32_t request[4] = {};
                const auto received = recv(sockets[1], request, sizeof(request), 0);
                if (received != sizeof(request))
                    std::cerr << "uinput recv=" << received << " errno=" << errno
                              << " expected="
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

    static void retroactiveWordHistory(Instance &instance) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager());
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlag::SurroundingText);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "skey-test";
        state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ =
            state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;

        state.viet_.setRawInput("chaof");
        state.committedLen_ = 4;
        state.saveLastWord(true);
        state.viet_.reset();
        state.committedLen_ = 0;

        state.viet_.setRawInput("cacs");
        state.committedLen_ = 3;
        state.saveLastWord(true);
        state.viet_.reset();
        state.committedLen_ = 0;
        check(state.retroHistory_.size() == 2,
              "retro history keeps more than the last committed word");

        bool reclaimed = state.reclaimLastWord();
        check(reclaimed && state.viet_.getRawInput() == "cacs",
              "reclaim restores the newest previous raw word");
        check(state.retroHistory_.size() == 1,
              "reclaim consumes only the newest history record");

        // Simulate the corrected word being committed again, then reclaim
        // once more.  The older word must remain available for the next
        // Backspace + tone-key cycle.
        state.viet_.reset();
        state.committedLen_ = 0;
        state.viet_.setRawInput("cacsj");
        state.committedLen_ = 3;
        state.saveLastWord(true);
        state.viet_.reset();
        state.committedLen_ = 0;
        check(state.reclaimLastWord() && state.viet_.getRawInput() == "cacsj",
              "corrected word is the next reclaim candidate");
        state.viet_.reset();
        state.committedLen_ = 0;
        check(state.reclaimLastWord() && state.viet_.getRawInput() == "chaof",
              "history supports reclaiming the preceding word in order");

        // A key that does not alter the reclaimed word must put the exact
        // record back; otherwise a later tone key would target a polluted
        // raw string containing that failed key.
        state.viet_.reset();
        state.committedLen_ = 0;
        state.viet_.setRawInput("banj");
        state.committedLen_ = 3;
        state.saveLastWord(true);
        state.viet_.reset();
        state.committedLen_ = 0;
        check(state.reclaimLastWord(), "reclaim accepts a final history word");
        state.viet_.processKey('q');
        state.restoreReclaimedWord();
        check(!state.retroHistory_.empty() &&
                  state.retroHistory_.back().raw == "banj",
              "failed reclaim transform restores the original raw record");
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

    // VS Code family: the DOM class of the focused input (xterm.js helper
    // textarea / Monaco input) decides terminal and editor regardless of
    // stale chat hints in the caps; the chat input keeps the caps verdict.
    static void ideWidgetAutoMode(Instance &instance, bool wayland) {
        using W = A11yMonitor::IdeWidget;
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(SKeyOutputMode::Auto);
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
        auto &monitor = *engine.a11yMonitor_;
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "code");
        input.setFocusGroup(&group);
        // Weak hint carried over from the chat panel (UppercaseWords).
        input.setCapabilityFlags(CapabilityFlags(0x80072));
        auto &state = *input.propertyFor(&engine.factory_);
        const auto detect = [&](W widget) {
            monitor.ideWidget_ = static_cast<int>(widget);
            monitor.focusInWebDoc_ = true;
            monitor.focusRole_ = 79;
            monitor.textEntryFocused_ = true;
            monitor.focusSingleLine_ = true;
            monitor.focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
            state.cachedProgram_ = "code";
            state.cachedIsChromium_ = 1;
            state.cachedIsTerminalApp_ = state.cachedIsFirefoxOrSnap_ =
                state.cachedIsFirefoxBrowser_ = 0;
            state.focusSawContentHints_ = true;
            state.modeCacheValid_ = false;
            return state.detectAutoMode();
        };
        const auto injecting = [](SKeyOutputMode mode) {
            return mode == SKeyOutputMode::Uinput || mode == SKeyOutputMode::Native;
        };
        check(injecting(detect(W::Terminal)),
              "xterm.js terminal class routes Uinput despite stale chat hints");
        check(state.a11yChromiumTerminal(), "terminal class counts as integrated terminal at webDoc=1");
        check(injecting(detect(W::CodeEditor)),
              "Monaco code editor routes Uinput despite stale chat hints");
        check(detect(W::Chat) == SKeyOutputMode::SurroundingText,
              "IDE chat input keeps the caps-based SurroundingText verdict");
        check(!state.a11yChromiumTerminal(), "chat input is never an integrated terminal");
        monitor.focusSnapshotUsec_ = 0;
        check(state.a11yIdeWidget() == W::Unknown, "stale snapshot never yields a widget kind");

        // Terminal latch: typing past the 5s freshness window in the same
        // terminal must not turn into "a11y blind" (delete-ack repair ate
        // real characters there).
        monitor.ideWidget_ = static_cast<int>(W::Terminal);
        monitor.focusBus_ = ":1.54";
        monitor.focusPath_ = "/org/a11y/atspi/accessible/49128";
        monitor.focusProcessId_ = 4242;
        monitor.focusGeneration_ = 7;
        monitor.focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
        state.a11yOwnerGeneration_ = 7;
        state.a11yOwnerPid_ = 4242;
        state.a11yOwnerProgram_ = "code";
        state.a11yOwnerMatches_ = true;
        check(state.ideTerminalFocused(), "fresh terminal focus latches");
        monitor.focusSnapshotUsec_ = now(CLOCK_MONOTONIC) - 6000000;
        check(state.a11yChromiumTerminal() && state.ideTerminalFocused(),
              "terminal verdict survives snapshot expiry on the same focus");
        monitor.focusGeneration_ = 8;
        check(!state.ideTerminalFocused(), "a new focus event drops the terminal latch");
        monitor.focusGeneration_ = 7;
        state.activate();
        check(!state.ideTerminalFocused(), "activation (app switch) drops the terminal latch");
        monitor.focusGeneration_ = 0;
    }

    // Chrome page search box (new-tab Google box) inline-autocompletes:
    // "cha" + selected suggestion tail.  One extra Backspace removes the
    // selection; without one the replacement leaves "chaào".
    // gnomeRelay: mutter relays Chrome as "gnome-shell" over the ibus
    // frontend; only the a11y focus (a Chromium process, web content)
    // identifies the page field.
    static void chromePageAutofill(Instance &instance, const std::string &text,
                                   unsigned cursor, unsigned anchor, uint32_t expectedBs,
                                   bool gnomeRelay = false) {
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        const std::string program = gnomeRelay ? "gnome-shell" : "google-chrome";
        TestInput input(instance.inputContextManager(), program);
        if (gnomeRelay) input.frontendName = "ibus";
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlags(0x72));
        // A real process whose binary sits next to chrome-sandbox — the
        // marker of renamed Chromium/Electron runtimes — owns the focus.
        char dirTemplate[] = "/tmp/skey-relay-XXXXXX";
        pid_t browser = -1;
        std::string browserDir;
        if (gnomeRelay) {
            check(mkdtemp(dirTemplate) != nullptr, "create relay browser fixture");
            browserDir = dirTemplate;
            std::filesystem::copy_file("/bin/sleep", browserDir + "/browser");
            std::ofstream(browserDir + "/chrome-sandbox") << "";
            browser = fork();
            if (browser == 0) {
                execl((browserDir + "/browser").c_str(), "browser", "30", nullptr);
                _exit(127);
            }
            check(browser > 0, "start relay browser fixture");
            for (int i = 0; i < 200 && skey::processExe(browser) != browserDir + "/browser"; ++i)
                usleep(1000);
            engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
            engine.a11yMonitor_->focusInWebDoc_ = true;
            engine.a11yMonitor_->focusProcessId_ = browser;
            engine.a11yMonitor_->focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
        }
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ =
            state.cachedIsFirefoxBrowser_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        state.viet_.setRawInput("cha");
        state.committedLen_ = 3;
        input.surroundingText().setText(text, cursor, anchor);
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "isolate the page-field replacement transport");
        captureUinput(state, sockets[0]);
        KeyEvent tone(&input, Key(FcitxKey_f));
        state.keyEvent(tone);
        uint32_t request[4] = {};
        if (recv(sockets[1], request, sizeof(request), 0) != sizeof(request) ||
            request[0] != expectedBs)
            std::cerr << "page autofill text='" << text << "' cursor=" << cursor
                      << " anchor=" << anchor << " sent=" << request[0] << '\n';
        check(request[0] == expectedBs && state.pendingUinputCommit_ == "à",
              "Chrome page field: one extra Backspace only for a selected autocomplete tail");
        state.resetForCellChange();
        close(sockets[1]);
        if (browser > 0) {
            kill(browser, SIGKILL);
            waitpid(browser, nullptr, 0);
            std::filesystem::remove_all(browserDir);
        }
    }

    // A word typed in the omnibox must not continue in the page: the
    // omnibox churn guard swallows the focus cycle of a real click into a
    // Google Sheets cell, so the reset has to come from the a11y focus.
    static void addrBarWordLeavesForPage(Instance &instance) {
        SKeyEngine engine(&instance, false);
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
        auto &monitor = *engine.a11yMonitor_;
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlags(CapabilityFlag::SurroundingText) |
                                 CapabilityFlag::Url);
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ =
            state.cachedIsFirefoxBrowser_ = 0;
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        const auto focus = [&](bool webContent, uint64_t ageUsec) {
            monitor.focusBus_ = ":1.35";
            monitor.focusPath_ = webContent ? "/sheets" : "/omnibox";
            monitor.focusInWebDoc_ = webContent;
            monitor.browserUIFocused_ = !webContent;
            monitor.focusSnapshotUsec_ = now(CLOCK_MONOTONIC) - ageUsec;
        };
        const auto wordThenFocus = [&](bool startInPage, bool webContent, uint64_t ageUsec) {
            state.viet_.reset();
            focus(startInPage, 0);
            state.checkCellSelection(); // word start
            state.viet_.setRawInput("gox");
            focus(webContent, ageUsec);
            state.checkCellSelection();
            return state.viet_.getRawInput().empty();
        };
        check(wordThenFocus(false, true, 400000),
              "omnibox word resets once the focus has settled in the page");
        check(!wordThenFocus(false, true, 50000),
              "a short bounce into the page (omnibox churn) keeps the omnibox word");
        check(!wordThenFocus(false, false, 400000), "focus back in the omnibox keeps the word");
        check(!wordThenFocus(true, true, 400000),
              "a word begun in the page (stale Url caps) never resets this way");
    }

    // Firefox URL bar inline autofill: "d" + selected "ocs.google.com/".
    // Shift+Left would shrink that selection ("dđ/"); the Backspace
    // transport with one extra BS removes it.  A mid-URL edit (tail
    // unchanged) must never get the extra Backspace.
    static void firefoxUrlAutofill(Instance &instance) {
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(SKeyOutputMode::Uinput);
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "firefox");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlags(CapabilityFlag::SurroundingText) |
                                 CapabilityFlag::Url);
        auto &state = *input.propertyFor(&engine.factory_);
        const auto prime = [&] {
            state.cachedProgram_ = "firefox";
            state.cachedIsChromium_ = state.cachedIsTerminalApp_ = 0;
            state.cachedIsFirefoxBrowser_ = state.cachedIsFirefoxOrSnap_ = 1;
            state.cachedWebKitProgram_ = "firefox";
            state.cachedIsWebKitGtk_ = 0;
        };
        const auto set = [&](const std::string &text, unsigned cursor) {
            input.surroundingText().setText(text, cursor, cursor);
        };
        const auto shown = [&](const std::string &startText, unsigned startCursor,
                               const std::string &nowText, unsigned nowCursor,
                               const std::string &word) {
            prime();
            set(startText, startCursor);
            const auto &surr = input.surroundingText();
            state.wordStartTailKnown_ = true;
            state.wordStartTail_ = surr.text().substr(static_cast<size_t>(
                utf8::ncharByteLength(surr.text().begin(), startCursor)));
            set(nowText, nowCursor);
            return state.firefoxUrlAutofillShown(word);
        };
        check(shown("", 0, "docs.google.com/", 1, "d"),
              "autofill after a word typed into an empty URL bar is detected");
        check(shown("https://example.com/", 0, "docs.google.com/", 1, "d"),
              "autofill after a word that replaced the selected URL (Ctrl+L) is detected");
        check(!shown("abcxyz", 3, "abcdxyz", 4, "d"),
              "mid-URL edit keeps its tail: never an autofill, never an extra Backspace");
        check(!shown("", 0, "d", 1, "d"), "no text after the caret is no autofill");
        check(!shown("", 0, "xxocs.google.com/", 2, "d"),
              "the typed word must sit right before the caret");
        state.wordStartTailKnown_ = false;
        set("docs.google.com/", 1);
        check(!state.firefoxUrlAutofillShown("d"), "unknown word-start state is never an autofill");

        // End to end: "dd" in an empty bar that autofills after "d".
        prime();
        state.cachedMode_ = SKeyOutputMode::Uinput;
        state.modeCacheValid_ = true;
        state.viet_.reset();
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "isolate the URL bar replacement transport");
        captureUinput(state, sockets[0]);
        set("", 0);
        KeyEvent first(&input, Key(FcitxKey_d));
        state.keyEvent(first);
        check(state.wordStartTailKnown_ && state.wordStartTail_.empty(),
              "the word start records the empty tail");
        set("docs.google.com/", 1);
        KeyEvent tone(&input, Key(FcitxKey_d));
        state.keyEvent(tone);
        uint32_t request[4] = {};
        check(recv(sockets[1], request, sizeof(request), 0) == sizeof(request) &&
                  request[0] == 3 && state.pendingUinputCommit_ == "đ",
              "autofill: one extra Backspace removes the selection, then đ replaces d");
        close(sockets[1]);
    }

    // Right after fcitx5 starts the uinput device is still warming up: the
    // first Chromium web replacement must not be forced onto it (rejected →
    // word reset, "đây" → "day"); the native surrounding path takes it.
    static void chromiumFirstReplaceUinputWarmup(Instance &instance, bool ready) {
        class Editor : public TestInput {
        public:
            using TestInput::TestInput;
            int deletes = 0;
        protected:
            void deleteSurroundingTextImpl(int, unsigned size) override {
                deletes += static_cast<int>(size);
            }
        };
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(SKeyOutputMode::SurroundingText);
        FocusGroup group("wayland:test", instance.inputContextManager());
        Editor input(instance.inputContextManager(), "google-chrome");
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlags(0x80072));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "google-chrome";
        state.cachedIsChromium_ = 1;
        state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ =
            state.cachedIsFirefoxBrowser_ = 0;
        state.cachedWebKitProgram_ = "google-chrome";
        state.cachedIsWebKitGtk_ = 0;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        state.modeCacheValid_ = true;
        state.uinputTestReady_ = ready;
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "isolate the warmup replacement transport");
        captureUinput(state, sockets[0]);
        KeyEvent first(&input, Key(FcitxKey_d));
        state.keyEvent(first);
        input.surroundingText().setText("d", 1, 1);
        KeyEvent tone(&input, Key(FcitxKey_d));
        state.keyEvent(tone);
        uint32_t request[4] = {};
        const bool injected = recv(sockets[1], request, sizeof(request), 0) == sizeof(request);
        if (ready) {
            check(injected && input.deletes == 0,
                  "ready uinput keeps the ordered first-replacement transport");
        } else {
            check(!injected && input.deletes == 1 && state.viet_.getComposed() == "đ",
                  "warming uinput: the first replacement rides the native delete, word kept");
            auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + 40000, 1,
                [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
            instance.eventLoop().exec();
            check(input.commits.size() == 1 && input.commits[0] == "đ",
                  "warming uinput: the replacement commits đ once");
        }
        close(sockets[1]);
    }

    // Wayland Chromium page churn: a Deactivate→Activate cycle while a
    // replacement awaits its settle must not commit into the disabled
    // text-input (dropped → Gemini lost every word); the held commit lands
    // in the reactivated IC.  A genuine focus loss discards it; other
    // Wayland apps keep the validated eager flush.
    // One event-loop run per Instance: a second exec() on the same loop
    // throws (stale handle) and aborts every later test.
    static void waylandBrowserChurn(Instance &instance, int scenario) {
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(SKeyOutputMode::SurroundingText);
        FocusGroup group("wayland:test", instance.inputContextManager());
        const auto prepare = [&](TestInput &input, const char *program, bool chromium) {
            input.setFocusGroup(&group);
            input.setCapabilityFlags(CapabilityFlags(0x80072));
            auto &state = *input.propertyFor(&engine.factory_);
            state.cachedProgram_ = program;
            state.cachedIsChromium_ = chromium ? 1 : 0;
            state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ =
                state.cachedIsFirefoxBrowser_ = 0;
            state.cachedMode_ = SKeyOutputMode::SurroundingText;
            state.modeCacheValid_ = true;
            // Replacement "d" -> "đ": Backspace already looped back, the
            // commit waits for the settle.
            state.viet_.reset();
            state.viet_.setRawInput("dd");
            state.uinputDeleting_ = true;
            state.uinputSettling_ = true;
            state.expectedUinputBackspaces_ = 0;
            state.pendingUinputCommit_ = "đ";
            state.uinputPendingFinalLen_ = 1;
            return &state;
        };
        const auto runLoop = [&](uint64_t usec) {
            auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + usec, 1,
                [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
            instance.eventLoop().exec();
        };

        if (scenario == 0) {
            TestInput chrome(instance.inputContextManager(), "google-chrome");
            auto &state = *prepare(chrome, "google-chrome", true);
            state.deactivate();
            check(chrome.commits.empty() && state.uinputDeleting_ &&
                      state.pendingUinputCommit_ == "đ" && state.uinputCycleTimer_,
                  "Wayland Chrome churn holds the replacement instead of committing into the disabled IC");
            state.reset();
            check(state.pendingUinputCommit_ == "đ", "the churn reset keeps the held replacement");
            state.finishUinputSettle(0, 0);
            check(chrome.commits.empty(), "no settle commit while the IC is deactivated");
            state.activate();
            check(!state.uinputCycleTimer_, "reactivation ends the churn window");
            state.finishUinputSettle(0, 0);
            runLoop(50000);
            check(chrome.commits.size() == 1 && chrome.commits[0] == "đ" &&
                      !state.uinputDeleting_,
                  "the held replacement commits exactly once into the reactivated IC");
        }
        if (scenario == 1) {
            TestInput chrome(instance.inputContextManager(), "google-chrome");
            auto &state = *prepare(chrome, "google-chrome", true);
            state.deactivate();
            runLoop(600000);
            check(chrome.commits.empty() && !state.uinputDeleting_ &&
                      state.pendingUinputCommit_.empty(),
                  "genuine focus loss discards the held replacement after 500ms");
        }
        if (scenario == 2) {
            TestInput telegram(instance.inputContextManager(), "Telegram");
            auto &state = *prepare(telegram, "Telegram", false);
            state.deactivate();
            check(telegram.commits.size() == 1 && telegram.commits[0] == "đ" &&
                      !state.uinputDeleting_,
                  "non-browser Wayland apps keep the eager deactivate flush");
        }
    }

    // The native-delete post-verify may mark an app delete-broken for the
    // whole session.  Only evidence about OUR replacement counts: a user
    // edit or a focus change ends the watch, and a browser's empty field
    // (composer cleared on send) is no evidence at all.
    static void surrNativeVerifyEvidence(Instance &instance) {
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(SKeyOutputMode::SurroundingText);
        FocusGroup group("wayland:test", instance.inputContextManager());
        const auto make = [&](const char *program, bool browser) {
            auto input = std::make_unique<TestInput>(instance.inputContextManager(), program);
            input->setFocusGroup(&group);
            input->setCapabilityFlags(CapabilityFlags(0x72));
            auto &state = *input->propertyFor(&engine.factory_);
            state.cachedProgram_ = program;
            state.cachedIsChromium_ = browser ? 1 : 0;
            state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ =
                state.cachedIsFirefoxBrowser_ = 0;
            state.cachedMode_ = SKeyOutputMode::SurroundingText;
            state.modeCacheValid_ = true;
            return input;
        };
        const auto pushed = [](const std::string &text) {
            fcitx::SurroundingText surrounding;
            const auto length = static_cast<unsigned>(utf8::length(text));
            surrounding.setText(text, length, length);
            return surrounding;
        };
        const auto arm = [](SKeyState &state) {
            state.armSurrNativePostVerify("go", "ô", "gô", 1, 2);
        };

        // Non-browser (areca-settings class): an empty push with no user
        // edit still marks the app — the original evidence is preserved.
        auto areca = make("areca-settings", false);
        auto &arecaState = *areca->propertyFor(&engine.factory_);
        arm(arecaState);
        check(arecaState.surrNativeRepairIfDropped(pushed(""), true, false) == 1 &&
                  engine.isSurrDeleteBrokenApp("areca-settings"),
              "non-browser empty push after a native replace still marks broken");
        engine.surrDeleteBrokenApps_.clear();

        // A user key that edits the field ends the watch.
        for (const Key userKey : {Key(FcitxKey_Return), Key(FcitxKey_KP_Enter),
                                  Key(FcitxKey_BackSpace), Key(FcitxKey_Delete),
                                  Key(FcitxKey_a, KeyState::Ctrl),
                                  Key(FcitxKey_x, KeyState::Ctrl)}) {
            arm(arecaState);
            arecaState.viet_.reset();
            KeyEvent event(areca.get(), userKey);
            arecaState.keyEvent(event);
            check(!arecaState.surrNativePostVerify_,
                  "a user edit (Enter/Backspace/Delete/Ctrl) ends the native-delete watch");
            check(arecaState.surrNativeRepairIfDropped(pushed(""), true, false) == 0 &&
                      !engine.isSurrDeleteBrokenApp("areca-settings"),
                  "the field the user emptied never marks the app broken");
        }
        // Typing letters keeps the watch (the lagging-push evidence).
        arm(arecaState);
        arecaState.viet_.reset();
        KeyEvent letter(areca.get(), Key(FcitxKey_g));
        arecaState.keyEvent(letter);
        check(arecaState.surrNativePostVerify_, "plain typing keeps the native-delete watch");
        // A focus change ends it too.
        arm(arecaState);
        arecaState.activate();
        check(!arecaState.surrNativePostVerify_, "activation ends the native-delete watch");
        engine.surrDeleteBrokenApps_.clear();

        // Browser: an empty field is no evidence (Gemini composer cleared).
        auto chrome = make("google-chrome", true);
        auto &chromeState = *chrome->propertyFor(&engine.factory_);
        arm(chromeState);
        check(chromeState.surrNativeRepairIfDropped(pushed(""), true, false) == 0 &&
                  !engine.isSurrDeleteBrokenApp("google-chrome") &&
                  !chromeState.surrNativePostVerify_,
              "browser empty field never marks the browser delete-broken");
        // ...but the text-shaped evidence still works for browsers.
        arm(chromeState);
        check(chromeState.surrNativeRepairIfDropped(pushed("goô"), true, false) == 1 &&
                  engine.isSurrDeleteBrokenApp("google-chrome"),
              "browser old word + committed suffix still proves a dropped delete");
        engine.surrDeleteBrokenApps_.clear();
    }

    // "A11y blind" means no trustworthy focus for this app — not "snapshot
    // older than 5s".  Typing past 5s in one live field must not switch on
    // the blind fallbacks; a dead/foreign/absent snapshot still must.
    static void a11yBlindDefinition(Instance &instance) {
        using W = A11yMonitor::IdeWidget;
        SKeyEngine engine(&instance, false);
        engine.config_.outputMode.setValue(SKeyOutputMode::Uinput);
        engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
        auto &monitor = *engine.a11yMonitor_;
        FocusGroup group("wayland:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), "google-chrome-stable");
        input.setFocusGroup(&group);
        auto &state = *input.propertyFor(&engine.factory_);
        const auto prime = [&] {
            state.cachedProgram_ = "google-chrome-stable";
            state.cachedIsChromium_ = 1;
            state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ =
                state.cachedIsFirefoxBrowser_ = 0;
            state.cachedMode_ = SKeyOutputMode::Uinput;
            state.modeCacheValid_ = true;
        };
        const auto snapshot = [&](uint64_t ageUsec, bool textEntry, W widget,
                                  int role) {
            monitor.focusBus_ = ":1.37";
            monitor.focusPath_ = "/org/a11y/atspi/accessible/2670";
            monitor.focusGeneration_ = 0;
            monitor.textEntryFocused_ = textEntry;
            monitor.ideWidget_ = static_cast<int>(widget);
            monitor.focusRole_ = role;
            monitor.focusSnapshotUsec_ = now(CLOCK_MONOTONIC) - ageUsec;
        };
        prime();

        // State matrix.
        monitor.focusSnapshotUsec_ = 0;
        check(state.a11yBlind(), "no focus snapshot at all is blind");
        snapshot(0, false, W::Unknown, 95);
        check(!state.a11yBlind(), "fresh snapshot is never blind");
        snapshot(6000000, true, W::Unknown, 79);
        check(!state.a11yBlind(), "same text entry past 5s stays trusted");
        snapshot(60000000, true, W::Unknown, 61);
        check(!state.a11yBlind(), "same text entry a minute later stays trusted");
        snapshot(6000000, false, W::Unknown, 95);
        check(state.a11yBlind(), "old window/document snapshot is no evidence about the field");
        snapshot(6000000, false, W::Terminal, 79);
        check(!state.a11yBlind(), "a classified IDE widget stays trusted past 5s");
        snapshot(0, true, W::Unknown, 79);
        monitor.focusGeneration_ = 3;
        monitor.focusProcessId_ = getpid(); // the test binary: not a Chrome process
        check(state.a11yBlind(), "focus owned by another application is blind");
        snapshot(6000000, true, W::Unknown, 79);
        monitor.invalidateFocus();
        check(state.a11yBlind(), "invalidated focus (monitor stopped) is blind");

        // Caret-jump cell-change fallback: inert on a live field past 5s,
        // active when blind.
        const auto jumpResets = [&](uint64_t age, bool textEntry) {
            prime();
            if (age == UINT64_MAX) monitor.focusSnapshotUsec_ = 0;
            else snapshot(age, textEntry, W::Unknown, textEntry ? 79 : 95);
            state.viet_.reset();
            state.viet_.setRawInput("chao");
            state.lastKeyCaretX_ = 100;
            state.lastKeyCaretY_ = 100;
            input.setCursorRect(Rect(400, 300, 401, 320));
            return state.checkCellSelection();
        };
        check(!jumpResets(6000000, true),
              "live field typed past 5s keeps the word on a caret jump");
        check(jumpResets(UINT64_MAX, false), "blind caret jump still resets the word");
        check(jumpResets(6000000, false), "old non-entry snapshot still counts as blind");

        // Surrounding field-change fallback: same gate.
        const auto fieldResets = [&](uint64_t age) {
            prime();
            if (age == UINT64_MAX) monitor.focusSnapshotUsec_ = 0;
            else snapshot(age, true, W::Unknown, 79);
            state.viet_.reset();
            state.viet_.setRawInput("chao");
            state.lastKeyCaretX_ = state.lastKeyCaretY_ = -1;
            state.surrPrevKeyUsec_ = now(CLOCK_MONOTONIC) - 300000;
            state.lastKeySurrText_ = "abc chao";
            state.lastKeySurrValid_ = true;
            input.surroundingText().setText("xyz", 0, 0);
            return state.checkBlindFieldChange();
        };
        check(!fieldResets(6000000), "live field past 5s: a surrounding mismatch is not a new field");
        check(fieldResets(UINT64_MAX), "blind: the new-field fallback still fires");

        // IDE chat composer: "bare caps after hints" must survive a long
        // typing session (a11y alive, snapshot old) but stay off when blind.
        TestInput ide(instance.inputContextManager(), "code");
        ide.setFocusGroup(&group);
        ide.setCapabilityFlags(CapabilityFlags(0x72));
        auto &ideState = *ide.propertyFor(&engine.factory_);
        const auto ideMode = [&](uint64_t age, W widget) {
            if (age == UINT64_MAX) monitor.focusSnapshotUsec_ = 0;
            else snapshot(age, true, widget, 79);
            monitor.focusInWebDoc_ = true;
            monitor.focusSingleLine_ = true;
            ideState.cachedProgram_ = "code";
            ideState.cachedIsChromium_ = 1;
            ideState.cachedIsTerminalApp_ = ideState.cachedIsFirefoxOrSnap_ =
                ideState.cachedIsFirefoxBrowser_ = 0;
            ideState.focusSawContentHints_ = true;
            ideState.modeDecisionPending_ = false;
            ideState.chromiumBareCapsUinput_ = false;
            ideState.modeCacheValid_ = false;
            return ideState.detectAutoMode();
        };
        const auto injecting = [](SKeyOutputMode mode) {
            return mode == SKeyOutputMode::Uinput || mode == SKeyOutputMode::Native;
        };
        check(ideMode(6000000, W::Chat) == SKeyOutputMode::SurroundingText,
              "chat composer keeps Surr after typing past 5s");
        check(injecting(ideMode(6000000, W::Terminal)),
              "terminal past 5s never takes the hints → Surr route");
        check(injecting(ideMode(6000000, W::CodeEditor)),
              "code editor past 5s stays on Uinput");
        check(injecting(ideMode(UINT64_MAX, W::Unknown)),
              "blind IDE skips the hints route (chat and terminal indistinguishable)");
        monitor.focusSnapshotUsec_ = 0;
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
                                 "com.example.chrome", "chrome-app-abcdef"}) {
            // firefox/zen are browsers with their own URL-bar modes since
            // 2026-10-27 (inFirefoxAddressBar) — not part of this list.
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
                // Since 2026-10-28 (waylandBrowserAddressBarCaret) a thin caret
                // near the top of the window is an address bar on Wayland even
                // without Url — some Chromium frontends omit it.  Known risk,
                // accepted after field use showed no problem: Chrome's Ctrl+F
                // find bar has the same caret shape (its Escape dismissal would
                // close the find bar).  Revisit here if that shows up.
                input.setCursorRect(Rect(10, 20, 11, 40));
                check(state.inChromiumAddressBar(),
                      "Wayland thin top caret without Url is treated as the address bar");
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

    // Wayland Surr-mode app that advertises SurroundingText but DROPS the
    // native delete_surrounding_text while commits land
    // Variants: neverPush (no surrounding pushes at all), mergedPush (the
    // replacement commit pushes with the next key already appended —
    // "goô" + "x"), emptyPush (pushes text='' after a replacement commit).
    // The first replace rides the native delete, the push proves the drop,
    // the word is repaired via uinput (or the app marked when no repair
    // evidence exists) and later replacements skip the native delete.
    static void surrNativeDroppedDelete(Instance &instance, bool neverPush,
                                        bool mergedPush = false,
                                        bool emptyPush = false) {
        class DropDeleteEditor : public TestInput {
        public:
            using TestInput::TestInput;
            std::string text;
            int droppedDeletes = 0;
        protected:
            void commitStringImpl(const std::string &value) override {
                TestInput::commitStringImpl(value);
                text += value;
                if (!neverPush_) {
                    std::string pushed = text;
                    if (value.size() > 1 && mergedPush_) pushed += "x";
                    if (value.size() > 1 && emptyPush_) pushed.clear();
                    const size_t pushedLen = utf8::length(pushed);
                    surroundingText().setText(pushed, pushedLen, pushedLen);
                }
            }
            void deleteSurroundingTextImpl(int, unsigned size) override {
                droppedDeletes += static_cast<int>(size); // dropped on purpose
            }
        public:
            bool neverPush_ = false;
            bool mergedPush_ = false;
            bool emptyPush_ = false;
        };
        SKeyEngine engine(&instance, false);
        FocusGroup group("wayland:test", instance.inputContextManager());
        DropDeleteEditor input(instance.inputContextManager(), "areca-settings");
        input.neverPush_ = neverPush;
        input.mergedPush_ = mergedPush;
        input.emptyPush_ = emptyPush;
        input.setFocusGroup(&group);
        input.setCapabilityFlags(CapabilityFlags(0x72));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = "areca-settings";
        state.cachedIsChromium_ = state.cachedIsFirefoxOrSnap_ = state.cachedIsTerminalApp_ = 0;
        state.modeCacheValid_ = true;
        state.cachedMode_ = SKeyOutputMode::SurroundingText;
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "isolate the replacement transport without desktop injection");
        captureUinput(state, sockets[0]);

        auto type = [&](fcitx::KeySym sym) {
            KeyEvent kev(&input, Key(sym));
            state.keyEvent(kev);
        };
        type(FcitxKey_g); // "g"
        type(FcitxKey_o); // "go"
        type(FcitxKey_o); // Telex: replace "go" -> "gô"
        if (neverPush) {
            check(input.droppedDeletes == 0,
                  "an app that never pushes surrounding must not ride the blind native delete");
        } else {
            check(input.droppedDeletes == 1 && input.text == "goô",
                  "the dropped native delete leaves the old word + committed suffix");
        }
        // The app's async push (dropDelete variants only) — arrives after
        // the verification was armed, like a real lagging push.  The
        // handle must be HELD: addTimeEvent returns a unique_ptr and
        // discarding it kills the timer.
        std::unique_ptr<EventSourceTime> pushTimer;
        if (!neverPush) {
            pushTimer = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + 2000, 1,
                [&input](EventSourceTime *, uint64_t) { input.updateSurroundingText(); return true; });
        }
        // Feed the injected BS loopbacks (real deletions + sync anchor)
        // and settle the replacement.
        auto feed = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
            now(CLOCK_MONOTONIC) + 5000, 1, [&](EventSourceTime *, uint64_t) {
                uint32_t request[4];
                if (recv(sockets[1], request, sizeof(request), 0) == sizeof(request)) {
                    for (uint32_t i = 0; i < request[0]; ++i) {
                        KeyEvent bs(&input, Key(FcitxKey_BackSpace));
                        state.keyEvent(bs);
                        if (i + 1 < request[0]) {
                            check(!bs.accepted(), "real deletion passes to the app");
                            size_t last = input.text.size() - 1;
                            while (last > 0 &&
                                   (static_cast<unsigned char>(input.text[last]) & 0xc0) == 0x80)
                                --last;
                            input.text.erase(last);
                        } else {
                            check(bs.accepted(), "sync anchor is consumed");
                        }
                    }
                    state.finishUinputSettle(0, 0);
                }
                return true;
            });
        auto stop = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
            now(CLOCK_MONOTONIC) + 80000, 1,
            [&](EventSourceTime *, uint64_t) { instance.eventLoop().exit(); return true; });
        instance.eventLoop().exec();
        if (neverPush) {
            check(input.text == "gô" && !state.uinputDeleting_, "word ends as composed");
            check(input.droppedDeletes == 0 && !engine.isSurrDeleteBrokenApp("areca-settings"),
                  "preemptive uinput transport leaves the app unmarked and uncorrupted");
        } else if (mergedPush || emptyPush) {
            check(engine.isSurrDeleteBrokenApp("areca-settings"),
                  "the merged/empty push still proves the drop and marks the app");
            check(input.droppedDeletes == 1 && input.text == "goô",
                  "no repair into the merged/empty evidence — the word stays broken once");
        } else {
            check(input.text == "gô" && !state.uinputDeleting_, "word ends as composed");
            check(engine.isSurrDeleteBrokenApp("areca-settings"),
                  "the proven drop marks the app delete-broken");
            check(input.commits == std::vector<std::string>{"g", "o", "ô", "ô"},
                  "the repair re-commits the suffix over the uinput deletion");
        }
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
    skey::UinputDeleteAck ack;
    constexpr uint64_t start = 100000;
    for (uint64_t delay = 1000; delay < ack.timeoutUsec; delay += 1000) {
        check(ack.start("xin tư!", 6, 6, "tư", 1, start), "track UTF-8 delete with prefix and tail");
        ack.observe(true, "xin t!", 5, 5, start); // not newer than send
        check(!ack.acknowledged(), "pre-send snapshot is not evidence");
        ack.observe(true, "xin t!", 5, 5, start + delay);
        ack.observe(true, "xin t!", 5, 5, start + delay + 1);
        check(ack.nextCheckDelay(start + delay) == ack.guardUsec &&
                  ack.nextCheckDelay(start + delay + ack.guardUsec) == 0,
              "event-driven acknowledgement retains exact guard deadline");
        check(ack.remaining(start + delay) == ack.guardUsec &&
                  ack.remaining(start + delay + ack.guardUsec) == 0,
              "fresh exact text waits one guard; duplicates do not prolong it");
    }
    check(ack.start("đây", 3, 3, "đây", 2, start), "multi-character UTF-8 deletion");
    ack.observe(true, "x", 1, 1, start + 1000);
    check(!ack.acknowledged(), "correct cursor with wrong text is not acknowledgement");
    ack.observe(true, "đ", 1, 0, start + 2000);
    check(!ack.acknowledged(), "selected text is not deletion acknowledgement");
    ack.observe(true, "đ", 1, 1, start + 3000);
    check(ack.acknowledged(), "exact multi-delete result acknowledges");
    check(ack.start("🙂đây!tail", 4, 9, "đây", 2, start),
          "UTF-8 prefix and autocomplete selection are trackable");
    ack.observe(true, "🙂đ!tail", 2, 7, start + 1000);
    check(ack.acknowledged(), "multi-character deletion preserves UTF-8 prefix and selected tail");
    check(ack.start("a🙂ớz", 3, 3, "🙂ớ", 2, start),
          "mixed four-byte and three-byte characters are trackable");
    ack.observe(true, "az", 1, 1, start + 1000);
    check(ack.acknowledged(), "deletion removes whole UTF-8 characters before the caret");
    ack.observe(false, "đ", 1, 1, start + 4000);
    check(!ack.acknowledged(), "new invalid snapshot revokes earlier acknowledgement");
    check(ack.nextCheckDelay(start + 4000) == ack.timeoutUsec - 4000,
          "invalidated acknowledgement rearms the bounded deadline, not polling");
    check(ack.remaining(start + ack.timeoutUsec - 1) == 1 &&
              ack.remaining(start + ack.timeoutUsec) == 0,
          "silent/invalid app has a bounded wait");
    check(!ack.start("tư", 2, 1, "tư", 1, start), "selection uses conservative fallback");
    check(!ack.start("t", 1, 1, "tư", 1, start), "stale original word uses fallback");
    check(!ack.start("tư", 9, 9, "tư", 1, start), "out of bounds cursor rejected");
    check(!ack.start(std::string(65537, 'x'), 65537, 65537, "x", 1, start),
          "large snapshots do not cause unbounded per-keystroke copies");
    check(ack.start("ư", 1, 1, "ư", 1, start), "deleting the whole word is trackable");
    ack.observe(true, "", 0, 0, start + 1000);
    check(ack.acknowledged(), "empty valid result acknowledges deletion");
    ack.reset();
    check(!ack.active() && ack.remaining(start + 1000) == 0, "cancelled observation cannot wait or acknowledge");
    ack.start("tư", 2, 2, "tư", 1, start);
    ack.observe(true, "t", 1, 1, start + 79000);
    check(ack.remaining(start + 80000) == 7000, "late acknowledgement still receives the full guard");
    ack.observe(true, "x", 1, 1, start + 81000);
    check(!ack.acknowledged() && ack.remaining(start + 81000) == 0,
          "conflicting update after deadline revokes evidence without extending timeout");
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
    A11yAncestorCache ancestors;
    ancestors.put(":1.54", "/a/10", {":1.54", "/a/9", 85, 1000});
    const auto *hit = ancestors.find(":1.54", "/a/10", 2000);
    check(hit && hit->parentPath == "/a/9" && hit->parentRole == 85, "known ancestor skips the round trips");
    check(!ancestors.find(":1.55", "/a/10", 2000), "ancestor cache must include sender");
    check(!ancestors.find(":1.54", "/a/10", 1000 + A11yAncestorCache::kTtlUsec),
          "ancestor entry expires after its TTL");
    ancestors.removeBus(":1.54");
    check(!ancestors.find(":1.54", "/a/10", 2000), "app leaving the bus drops its ancestors");

    // Runtime markers, including a sandbox root (flatpak: the exe path is
    // inside the app's own mount namespace, probed via /proc/<pid>/root).
    {
        char rootTemplate[] = "/tmp/skey-markers-XXXXXX";
        const char *root = mkdtemp(rootTemplate);
        check(root != nullptr, "create marker fixture root");
        const auto touch = [&](const std::string &file) {
            std::filesystem::create_directories(std::filesystem::path(root + file).parent_path());
            std::ofstream(root + file) << "";
        };
        touch("/app/lib/firefox/libxul.so");
        touch("/app/extra/apps/chrome-sandbox");
        check(skey::exeHasGeckoMarker("/app/lib/firefox/firefox", root) &&
                  !skey::exeHasGeckoMarker("/app/lib/firefox/firefox"),
              "Gecko marker is found inside the sandbox root, not on the host path");
        check(skey::exeHasChromiumMarkers("/app/extra/apps/renamed-shell", root) &&
                  !skey::exeHasChromiumMarkers("/app/extra/apps/renamed-shell"),
              "Chromium helper binaries are found inside the sandbox root");
        check(skey::exeHasChromiumMarkers("/opt/google/chrome/chrome") &&
                  skey::exeHasChromiumMarkers("/usr/lib/Electron/app"),
              "a runtime named in the exe path needs no file probe");
        check(!skey::exeHasChromiumMarkers("/usr/bin/konsole", root) &&
                  !skey::exeHasGeckoMarker("/usr/bin/telegram-desktop", root),
              "native apps carry no runtime marker");
        std::filesystem::remove_all(root);
    }
}

int main(int argc, char **argv) {
    // activate() reloads per-app modes. Never inherit the test machine's
    // interactive Chrome settings (e.g. a forced Surrounding Text mode).
    char configTemplate[] = "/tmp/skey-engine-test-XXXXXX";
    const char *configRoot = mkdtemp(configTemplate);
    check(configRoot != nullptr, "create isolated test configuration");
    struct ConfigCleanup {
        std::string path;
        ~ConfigCleanup() { std::filesystem::remove_all(path); }
    } configCleanup{configRoot};
    setenv("XDG_CONFIG_HOME", configRoot, 1);
    setenv("XDG_CONFIG_DIRS", configRoot, 1);
    // Synthetic caret coordinates must not inherit a developer desktop's
    // active-window origin or DPI. Live X11 geometry is tested separately.
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    // The session type decides native X11 vs XWayland (waylandSession()):
    // run on a Wayland desktop, every "x11:test" case was routed as
    // XWayland and the X11 expectations failed.  Each case declares its
    // platform through the input's display; GNOME cases set the desktop
    // explicitly below (gnomeDesktop() caches its first answer).
    unsetenv("XDG_SESSION_TYPE");
    unsetenv("XDG_CURRENT_DESKTOP");
    // Never reach the developer's desktop.  A Native route starts the libei
    // injector, which asked the REAL RemoteDesktop portal for keyboard
    // permission on every run (a popup each time, 2026-10-31); and the debug
    // flag is read from $HOME, so cases wrote into the live /tmp/skey.log.
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/skey-test-bus", 1);
    setenv("HOME", configRoot, 1);
    {
        const auto confDir = std::filesystem::path(configRoot) / ".config" / "fcitx5" / "conf";
        std::filesystem::create_directories(confDir);
        std::ofstream(confDir / "skey.conf") << "Debug=False\n";
    }

    if (std::getenv("SKEY_TEST_GNOME_NATIVE_ONLY")) {
        setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::gnomeOmniboxReplacement(instance, true);
        fcitx::EnginePerformanceTest::gnomeOmniboxAppendAfterTimeout(instance, true);
        fcitx::EnginePerformanceTest::gnomeOmniboxEditing(instance, true);
        std::cout << "GNOME omnibox Native: " << checks << " checks passed\n";
        return 0;
    }

    if (std::getenv("SKEY_TEST_GNOME_UINPUT_ONLY")) {
        setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::gnomeOmniboxReplacement(instance);
        fcitx::EnginePerformanceTest::gnomeOmniboxAppendAfterTimeout(instance);
        std::cout << "GNOME omnibox Uinput: " << checks << " checks passed\n";
        return 0;
    }

    if (std::getenv("SKEY_TEST_GNOME_EDIT_ONLY")) {
        setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::gnomeOmniboxEditing(instance);
        std::cout << "GNOME omnibox editing: " << checks << " checks passed\n";
        return 0;
    }

    if (std::getenv("SKEY_TEST_GNOME_MENU_ONLY")) {
        setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::gnomeAddressBarMenu(instance);
        std::cout << "GNOME address-bar menu: " << checks << " checks passed\n";
        return 0;
    }

    if (std::getenv("SKEY_TEST_X11_SELECT_ONLY")) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::addressBarSelectAnchor(instance);
        std::cout << "X11 Select replacement: " << checks << " checks passed\n";
        return 0;
    }

    // Keep the focused retroactive-editing regression ahead of the large
    // integration matrix so it remains visible if an unrelated baseline
    // check stops the full performance test early.
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::retroactiveWordHistory(instance);
    }
    if (std::getenv("SKEY_TEST_RETRO_ONLY")) {
        std::cout << "Retroactive word history: " << checks
                  << " checks passed\n";
        return 0;
    }

    for (bool uinput : {false, true}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::blindA11yFieldReset(instance, uinput);
    }
    for (bool settling : {false, true}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::uinputPendingTransport(instance, settling);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::uinputFailures(instance);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::replacementMenus(instance);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::surrNativeDroppedDelete(instance, false);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::surrNativeDroppedDelete(instance, true);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::surrNativeDroppedDelete(instance, false,
                                                              true);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::surrNativeDroppedDelete(instance, false,
                                                              false, true);
    }
    for (auto mode : {fcitx::SKeyOutputMode::Uinput, fcitx::SKeyOutputMode::Native}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::injectedBackspaceContract(instance, mode);
    }
    for (auto mode : {fcitx::SKeyOutputMode::Uinput, fcitx::SKeyOutputMode::Native}) {
        for (bool settling : {false, true}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::nativeAddressBarChurn(instance, mode, settling);
        }
    }
    if (std::getenv("SKEY_TEST_BACKSPACE_ONLY")) {
        std::cout << "Injected Backspace contract: " << checks << " checks passed\n";
        return 0;
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::webkitToolkitDetection(instance);
    }
    for (bool wayland : {false, true})
      for (bool perApp : {false, true})
        for (const char *keys : {"chafo", "chaof"}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::webkitSurrounding(instance, wayland, perApp, keys);
        }
    if (std::getenv("SKEY_TEST_WEBKIT_ONLY")) {
        std::cout << "WebKit Surrounding Text: " << checks << " checks passed\n";
        return 0;
    }
    for (bool native : {false, true}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::omniboxSurrounding(instance, native, true);
    }
    if (std::getenv("SKEY_TEST_WAYLAND_OMNIBOX_ONLY")) {
        std::cout << "Wayland omnibox Surrounding Text: " << checks << " checks passed\n";
        return 0;
    }
    for (bool native : {false, true}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::omniboxSurrounding(instance, native, false);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::omniboxSurrounding(instance, true, false, true);
    }
    if (std::getenv("SKEY_TEST_X11_OMNIBOX_ONLY")) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::addressBarSuffixReplacement(instance);
        fcitx::EnginePerformanceTest::addressBarSuffixReplacement(instance, true);
        std::cout << "X11 omnibox Surrounding Text: " << checks << " checks passed\n";
        return 0;
    }
    policies();
    for (bool wayland : {false, true})
      for (bool uinput : {false, true})
        for (bool shortW : {false, true})
          for (char key : {'w', 'W', 'c'}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::chromiumFirstShortW(instance, wayland, uinput, shortW, key);
          }
    for (bool wayland : {false, true}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::firefoxSuffixTransaction(instance, wayland);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::surroundingMeasurement(instance);
    }
    for (bool wayland : {false, true})
        for (bool native : {false, true}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::surroundingDeadline(instance, wayland, native);
        }
    for (bool replay : {false, true})
        for (bool timestamped : {false, true}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::waylandRepeatedLetter(instance, replay, timestamped);
        }
    for (bool firefox : {false, true}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::x11AckBeforeAnchor(instance, firefox);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::x11BrowserFirstSettle(instance);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::addressBarSelectAnchor(instance);
    }
    for (const auto &suffix : {"c", " ", "ư", "ự"})
        for (bool wayland : {false, true}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::chromiumCompositionCommit(
                instance, wayland, true, false, "tabby", false, 0, false, suffix);
        }

    for (int deletes : {1, 2, 3})
        for (int overrideMs : {-1, 7}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::tabbyTimingAndQueue(instance, "tabby", false, deletes, overrideMs);
        }
    for (const auto &program : {"code", "google-chrome-stable"}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::tabbyTimingAndQueue(instance, program, false, 1, -1);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::tabbyTimingAndQueue(instance, "tabby", true, 1, -1);
    }
    for (bool pending : {false, true})
        for (bool replay : {false, true}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::chromeFastRepeat(instance, pending, replay, "tabby");
        }

    for (bool sheets : {false, true}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumX11RepeatedTone(instance, sheets);
    }
    for (int snapshot : {0, 1, 2, 3})
        for (char tone : {'j', 'f'}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::chromiumX11SurroundingFallback(instance, snapshot, tone);
        }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "google-chrome", true, -1, false, true);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, false, "google-chrome", true, -1, false, true);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, false, "google-chrome", false, -1, false, false);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, false, "google-chrome", true, 7, false, false);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, false, "google-chrome", true, -1, true, false);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, false, "code", true, -1, false, false);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, false, "google-chrome", true, -1, false, false, true);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, false, "google-chrome", true, -1, false, false, false, true);
        // Wayland verifies Chromium-family deletes even with manual timing
        // or Url hints; the commit stage still honors a manual delay.
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "google-chrome", false, -1, false, true);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "google-chrome", true, 7, false, true);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "google-chrome", true, -1, true, true);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "antigravity-ide", true, -1, false, true);
    }
    for (bool wayland : {false, true})
        for (bool cap : {false, true})
            for (bool url : {false, true})
                for (bool replacement : {false, true})
                    for (const char *program : {"google-chrome", "code", "firefox"}) {
                        fcitx::Instance instance(argc, argv);
                        fcitx::EnginePerformanceTest::chromiumCompositionCommit(
                            instance, wayland, cap, url, program, replacement);
                    }
    for (bool wayland : {false, true})
      for (int sheets : {1, 2}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumCompositionCommit(
            instance, wayland, true, false, "google-chrome", true, sheets);
    }
    for (bool wayland : {false, true})
      for (const std::string suffix : {"ào", "ạn", "ồi"})
        for (int scope = 0; scope < 6; ++scope) {
          // Cursor geometry identifies the omnibox only on X11. Wayland's
          // explicit URL capability exclusion is covered by scope 2.
          if (wayland && scope == 4) continue;
          fcitx::Instance instance(argc, argv);
          fcitx::EnginePerformanceTest::chromiumCompositionCommit(
              instance, wayland, scope != 1, scope == 2,
              scope == 5 ? "firefox" : "google-chrome", true,
              scope == 3 ? 1 : 0, scope == 4, suffix);
        }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumCompositionCommit(
            instance, false, true, false, "google-chrome", true, 0, true);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumCompositionCommit(
            instance, false, true, false, "firefox", true, 3);
    }
    for (char tone : {'j', 'f'})
        for (bool suffixPath : {false, true})
            for (int delay : {1, 15, 40, 70, -1, -2, -3}) {
                fcitx::Instance instance(argc, argv);
                fcitx::EnginePerformanceTest::chromiumDeletionAck(
                    instance, delay, suffixPath, false, tone, true);
            }
    for (char tone : {'j', 'f'})
        for (int delay : {15, 40, -1, -3}) {
            fcitx::Instance instance(argc, argv);
            fcitx::EnginePerformanceTest::chromiumDeletionAck(instance, delay, false, true, tone);
        }
    for (int delay : {1, 5, 15, 40, 70, -1, -2, -3}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumDeletionAck(instance, delay);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumDeletionAck(instance, 15, true);
    }
    for (int snapshot : {0, 1, 2}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumRendererSettle(instance, snapshot);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumRendererSettle(instance, 0, 7);
    }
    {
        fcitx::Instance instance(argc, argv);
        for (bool wayland : {false, true})
            for (bool client : {false, true})
                fcitx::EnginePerformanceTest::preeditVisibility(instance, wayland, client);
        for (int snapshot : {0, 1, 2, 3})
            fcitx::EnginePerformanceTest::firefoxSelectAllBackspace(instance, snapshot);
        fcitx::EnginePerformanceTest::addressBarSuffixReplacement(instance);
        fcitx::EnginePerformanceTest::addressBarSuffixReplacement(instance, true);
        fcitx::EnginePerformanceTest::addressBarSettleAcrossFocus(instance);
        for (bool pending : {false, true})
            for (bool replay : {false, true})
                fcitx::EnginePerformanceTest::chromeFastRepeat(instance, pending, replay);
        fcitx::EnginePerformanceTest::addressBarCycleExpiry(instance);
        fcitx::EnginePerformanceTest::addressBarCycleExpiry(instance, true);
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
        fcitx::EnginePerformanceTest::ideWidgetAutoMode(instance, false);
        fcitx::EnginePerformanceTest::ideWidgetAutoMode(instance, true);
        fcitx::EnginePerformanceTest::a11yBlindDefinition(instance);
        fcitx::EnginePerformanceTest::surrNativeVerifyEvidence(instance);
    }
    for (int scenario : {0, 1, 2}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::waylandBrowserChurn(instance, scenario);
    }
    for (bool ready : {false, true}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumFirstReplaceUinputWarmup(instance, ready);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::firefoxUrlAutofill(instance);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::addrBarWordLeavesForPage(instance);
    }
    {
        fcitx::Instance instance(argc, argv);
        // Autocomplete tail selected after the typed word: +1 BS.
        fcitx::EnginePerformanceTest::chromePageAutofill(instance, "chatgpt.com", 3, 11, 3);
        // No selection: the plain deletion plus anchor.
        fcitx::EnginePerformanceTest::chromePageAutofill(instance, "cha", 3, 3, 2);
        // A selection that does not reach the end is not an autocomplete.
        fcitx::EnginePerformanceTest::chromePageAutofill(instance, "chatgpt.com", 3, 5, 2);
        // GNOME: the same fields relayed as gnome-shell.
        fcitx::EnginePerformanceTest::chromePageAutofill(instance, "chatgpt.com", 3, 11, 3, true);
        fcitx::EnginePerformanceTest::chromePageAutofill(instance, "cha", 3, 3, 2, true);
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
    for (bool uinput : {false, true}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::blindA11yFieldReset(instance, uinput);
    }
    std::cout << "Engine performance: " << checks << " checks passed\n";
}
