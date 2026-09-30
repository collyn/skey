#include "engine.h"
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
        KeyEvent event(&input, Key(FcitxKey_a), false,
                       timestamped ? (replay ? 1000 : 1020) : 0);
        state.keyEvent(event);
        check(state.viet_.getComposed() == (!replay && timestamped ? "đâ" : "đa"),
              "Wayland fresh repeat composes aa; identical or missing timestamps retain guard");
        state.resetForCellChange();
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
            check(input.commits == std::vector<std::string>{"ự"} && input.forwarded.size() == 3 &&
                      input.forwarded[0].key.check(FcitxKey_space) && !input.forwarded[0].release &&
                      input.forwarded[1].key.check(FcitxKey_n) && !input.forwarded[1].release &&
                      input.forwarded[2].key.check(FcitxKey_n) && input.forwarded[2].release &&
                      input.forwarded[1].time == 2010 && input.forwarded[2].time == 2011,
                  "Tabby forwards queued ASCII keys and releases with original identity and order");
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
        state.uinputClientFd_ = sockets[0];
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
        close(sockets[0]);
        state.uinputClientFd_ = -1;
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
        state.uinputClientFd_ = sockets[0];
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
        close(sockets[0]);
        state.uinputClientFd_ = -1;
        close(sockets[1]);
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
        const bool expected = preeditCap && !url && !omnibox && sheets != 1 &&
            ((!wayland && std::string(program) == "tabby") ||
             (replacement && ((std::string(program) == "google-chrome") ||
              (!wayland && firefox && sheets != 3)))) &&
            (std::string(program) == "google-chrome" || utf8::length(suffix) == 1);
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
                                 int overrideMs, bool url, bool expected,
                                 bool sheets = false, bool omnibox = false) {
        SKeyEngine engine(&instance, false);
        engine.config_.autoDelay.setValue(autoDelay);
        if (sheets) {
            engine.a11yMonitor_ = std::make_unique<A11yMonitor>();
            engine.a11yMonitor_->sheetsEditorFocused_ = true;
            engine.a11yMonitor_->focusSnapshotUsec_ = now(CLOCK_MONOTONIC);
        }
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
        state.uinputClientFd_ = sockets[0];
        state.sendBackspaceUinput(2, 0, "tư");
        check(state.uinputDeleteAck_.active() == expected,
              "ack timing is limited to automatic browser page replacements");
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
        state.uinputClientFd_ = sockets[0];
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
        close(sockets[0]);
        state.uinputClientFd_ = -1;
        close(sockets[1]);
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
