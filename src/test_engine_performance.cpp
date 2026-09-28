#include "engine.h"
#include "input_timing.h"
#include "uinput_delete_ack.h"
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
    static void chromiumCompositionCommit(Instance &instance, bool wayland,
                                          bool preeditCap, bool url,
                                          const char *program, bool replacement, int sheets = 0) {
        // Model Chromium's NeedInsertChar branch, not Facebook itself.
        class CompositionInput : public TestInput {
        public:
            CompositionInput(InputContextManager &manager, const char *program)
                : TestInput(manager, program) {}
            std::string composition, visible;
            std::vector<std::string> events;
        protected:
            void updatePreeditImpl() override {
                composition = inputPanel().clientPreedit().toString();
                events.push_back("preedit:" + composition);
            }
            void commitStringImpl(const std::string &text) override {
                events.push_back("commit:" + text);
                // Adversarial editor ignores the single-character key route.
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
        input.setCapabilityFlags((preeditCap ? CapabilityFlags(CapabilityFlag::Preedit) : CapabilityFlags()) |
                                 (url ? CapabilityFlags(CapabilityFlag::Url) : CapabilityFlags()));
        auto &state = *input.propertyFor(&engine.factory_);
        state.cachedProgram_ = program;
        const bool firefox = std::string(program) == "firefox";
        state.cachedIsChromium_ = firefox ? 0 : 1;
        state.cachedIsFirefoxOrSnap_ = firefox ? 1 : 0;
        state.cachedIsTerminalApp_ = 0;
        const bool expected = preeditCap && !url && replacement && sheets != 1 &&
            ((wayland && std::string(program) == "google-chrome") ||
             (!wayland && firefox && sheets != 3));
        check(state.commitText("ự", replacement), "replacement commit issued");
        check(input.commits == std::vector<std::string>{"ự"}, "single suffix commit without retries");
        check(input.visible == (expected ? "ự" : ""), "composition selects text insertion instead of character key route");
        check(input.events == (expected ? std::vector<std::string>{"preedit:ự", "commit:ự", "preedit:"}
                                        : std::vector<std::string>{"commit:ự"}),
              "composition must precede commit and be cleared afterwards only in scoped route");
        check(input.inputPanel().clientPreedit().empty(), "no lingering composition even when Show preedit is off");
    }

    static void chromiumDeletionAck(Instance &instance, int ackDelayMs, bool suffixPath = false, bool firefox = false, char tone = 'j') {
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
        FocusGroup group(firefox ? "x11:test" : "wayland:test", instance.inputContextManager());
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
        engine.noteAppRoundTrip(program, !firefox, 1000);
        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0,
              "create isolated Chrome deletion transport");
        state.uinputClientFd_ = sockets[0];
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
        const auto key = skey::appDelayKey(program, !firefox);
        check(engine.appDelayStats_.at(key).lastSleepUsec ==
                  skey::UinputDeleteAck::guardUsec,
              "observed path starts at 8ms, not the unconditional 30ms floor");
        KeyEvent navigation(&input, Key(FcitxKey_End));
        state.keyEvent(navigation);
        check(navigation.accepted() && input.forwarded.empty(), "subsequent keys wait behind observed deletion");
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
            check(state.uinputAckUnavailable_ == (ackDelayMs < 0),
                  "only missing/unusable acknowledgements disable further waits for this focus");
            if (ackDelayMs < 0) {
                input.surroundingText().setText("xin tư!", 6, 6);
                input.updateSurroundingText();
                state.sendBackspaceUinput(2, 0, "tư");
                check(!state.uinputDeleteAck_.active(), "silent input must not pay 80ms on every replacement");
            }
        }
        close(sockets[0]);
        state.uinputClientFd_ = -1;
        close(sockets[1]);
    }

    static void chromiumAckScope(Instance &instance, bool wayland,
                                 const char *program, bool autoDelay,
                                 int overrideMs, bool url, bool expected) {
        SKeyEngine engine(&instance, false);
        engine.config_.autoDelay.setValue(autoDelay);
        FocusGroup group(wayland ? "wayland:test" : "x11:test", instance.inputContextManager());
        TestInput input(instance.inputContextManager(), program);
        input.setFocusGroup(&group);
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
        state.uinputClientFd_ = sockets[0];
        state.sendBackspaceUinput(2, 0, "tư");
        check(state.uinputDeleteAck_.active() == expected,
              "ack timing is limited to automatic Wayland browser page replacements");
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
        close(sockets[0]);
        state.uinputClientFd_ = -1;
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
        // to accept the replacement. No real Uinput server is involved.
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
    skey::UinputDeleteAck ack;
    constexpr uint64_t start = 100000;
    for (uint64_t delay = 1000; delay < ack.timeoutUsec; delay += 1000) {
        check(ack.start("xin tư!", 6, 6, "tư", 1, start), "track UTF-8 delete with prefix and tail");
        ack.observe(true, "xin t!", 5, 5, start); // not newer than send
        check(!ack.acknowledged(), "pre-send snapshot is not evidence");
        ack.observe(true, "xin t!", 5, 5, start + delay);
        ack.observe(true, "xin t!", 5, 5, start + delay + 1);
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
    ack.observe(false, "đ", 1, 1, start + 4000);
    check(!ack.acknowledged(), "new invalid snapshot revokes earlier acknowledgement");
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
}

int main(int argc, char **argv) {
    policies();
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "google-chrome", true, -1, false, true);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, false, "google-chrome", true, -1, false, false);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "google-chrome", false, -1, false, false);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "google-chrome", true, 7, false, false);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "google-chrome", true, -1, true, false);
        fcitx::EnginePerformanceTest::chromiumAckScope(instance, true, "antigravity-ide", true, -1, false, false);
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
    for (int sheets : {1, 2}) {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumCompositionCommit(
            instance, true, true, false, "google-chrome", true, sheets);
    }
    {
        fcitx::Instance instance(argc, argv);
        fcitx::EnginePerformanceTest::chromiumCompositionCommit(
            instance, false, true, false, "firefox", true, 3);
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
