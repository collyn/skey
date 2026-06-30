#include "engine.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputpanel.h>
#include <fcitx/statusarea.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <ctime>

class SKeyLogger {
public:
    ~SKeyLogger() {
        std::ofstream f("/tmp/skey.log", std::ios::app);
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
        f << "[" << buf << "] " << ss_.str() << std::endl;
    }
    template <typename T>
    SKeyLogger &operator<<(const T &v) { ss_ << v; return *this; }
private:
    std::ostringstream ss_;
};

namespace fcitx {

static bool isUtf8ContinuationByte(char ch) {
    return (static_cast<unsigned char>(ch) & 0xC0) == 0x80;
}

static size_t commonUtf8PrefixBytes(const std::string &a,
                                    const std::string &b) {
    size_t prefix = 0;
    size_t limit = std::min(a.size(), b.size());
    while (prefix < limit && a[prefix] == b[prefix]) {
        ++prefix;
    }
    while (prefix > 0 && prefix < a.size() && isUtf8ContinuationByte(a[prefix])) {
        --prefix;
    }
    while (prefix > 0 && prefix < b.size() && isUtf8ContinuationByte(b[prefix])) {
        --prefix;
    }
    return prefix;
}


FCITX_DEFINE_LOG_CATEGORY(skey_log, "skey");
#define SKEY_DEBUG() SKeyLogger()
#define SKEY_INFO() SKeyLogger()

FCITX_ADDON_FACTORY(SKeyEngineFactory);

// Candidate word for mode switch dropdown menu
class ModeCandidateWord : public CandidateWord {
public:
    ModeCandidateWord(SKeyEngine *engine, SKeyState *state,
                      const std::string &text, SKeyOutputMode mode)
        : CandidateWord(Text(text)), engine_(engine), state_(state),
          mode_(mode) {}

    void select(InputContext *) const override {
        state_->appModeOverride_ = mode_;
        state_->hasAppModeOverride_ = true;
        engine_->saveAppMode(state_->ic_->program(), mode_);
        SKEY_INFO() << "Mode switched to "
                    << (mode_ == SKeyOutputMode::SurroundingText
                            ? "SurroundingText"
                            : (mode_ == SKeyOutputMode::Preedit
                                   ? "Preedit"
                                   : "SurroundingTextSlow"));
        state_->dismissModeMenu();
    }

private:
    SKeyEngine *engine_;
    SKeyState *state_;
    SKeyOutputMode mode_;
};

// ---------------------------------------------------------------------------
// SKeyEngine
// ---------------------------------------------------------------------------

SKeyEngine::SKeyEngine(Instance *instance)
    : instance_(instance),
      factory_([this](InputContext &ic) -> SKeyState * {
          return new SKeyState(this, &ic);
      }) {
    reloadConfig();
    instance_->inputContextManager().registerProperty("skeyState", &factory_);
    setupTrayMenu();
    SKEY_INFO() << "SKey Vietnamese Input Method loaded";
}

void SKeyEngine::setupTrayMenu() {
    auto &uiManager = instance_->userInterfaceManager();

    // ── Input Method menu ──
    imTelex_.setShortText("Telex");
    imTelex_.setCheckable(true);
    imTelex_.registerAction("skey-im-telex", &uiManager);
    imVni_.setShortText("VNI");
    imVni_.setCheckable(true);
    imVni_.registerAction("skey-im-vni", &uiManager);
    imTelexW_.setShortText("Telex W");
    imTelexW_.setCheckable(true);
    imTelexW_.registerAction("skey-im-telexw", &uiManager);

    imMenu_.addAction(&imTelex_);
    imMenu_.addAction(&imVni_);
    imMenu_.addAction(&imTelexW_);

    imAction_.setShortText(_("Input Method"));
    imAction_.setMenu(&imMenu_);
    imAction_.registerAction("skey-input-method", &uiManager);

    imTelex_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        FCITX_UNUSED(ic);
        setInputMethod(SKeyInputMethod::Telex);
    });
    imVni_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        FCITX_UNUSED(ic);
        setInputMethod(SKeyInputMethod::VNI);
    });
    imTelexW_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        FCITX_UNUSED(ic);
        setInputMethod(SKeyInputMethod::TelexW);
    });

    // ── Output Mode menu ──
    omSurrounding_.setShortText(_("Surrounding Text"));
    omSurrounding_.setCheckable(true);
    omSurrounding_.registerAction("skey-om-surrounding", &uiManager);
    omPreedit_.setShortText(_("Preedit"));
    omPreedit_.setCheckable(true);
    omPreedit_.registerAction("skey-om-preedit", &uiManager);
    omSurroundingSlow_.setShortText(_("Surrounding Text (slow)"));
    omSurroundingSlow_.setCheckable(true);
    omSurroundingSlow_.registerAction("skey-om-surrounding-slow", &uiManager);

    omMenu_.addAction(&omSurrounding_);
    omMenu_.addAction(&omPreedit_);
    omMenu_.addAction(&omSurroundingSlow_);

    omAction_.setShortText(_("Output Mode"));
    omAction_.setMenu(&omMenu_);
    omAction_.registerAction("skey-output-mode", &uiManager);

    omSurrounding_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        FCITX_UNUSED(ic);
        setOutputMode(SKeyOutputMode::SurroundingText);
    });
    omPreedit_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        FCITX_UNUSED(ic);
        setOutputMode(SKeyOutputMode::Preedit);
    });
    omSurroundingSlow_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        FCITX_UNUSED(ic);
        setOutputMode(SKeyOutputMode::SurroundingTextSlow);
    });

    updateMenuActions();
}

void SKeyEngine::keyEvent(const InputMethodEntry &entry,
                          KeyEvent &keyEvent) {
    FCITX_UNUSED(entry);
    auto *state = keyEvent.inputContext()->propertyFor(&factory_);
    if (state) {
        state->keyEvent(keyEvent);
    }
}

void SKeyEngine::activate(const InputMethodEntry &entry,
                          InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *ic = event.inputContext();

    // Add tray menu actions (InputMethod group is cleared before activate)
    ic->statusArea().addAction(StatusGroup::InputMethod, &imAction_);
    ic->statusArea().addAction(StatusGroup::InputMethod, &omAction_);
    updateMenuActions();

    auto *state = ic->propertyFor(&factory_);
    if (state) {
        state->activate();
    }
}

void SKeyEngine::deactivate(const InputMethodEntry &entry,
                            InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *state = event.inputContext()->propertyFor(&factory_);
    if (state) {
        state->deactivate();
    }
}

void SKeyEngine::reset(const InputMethodEntry &entry,
                       InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *state = event.inputContext()->propertyFor(&factory_);
    if (state) {
        state->reset();
    }
}

void SKeyEngine::save() {}

const Configuration *SKeyEngine::getConfig() const { return &config_; }

void SKeyEngine::setConfig(const RawConfig &config) {
    config_.load(config, true);
    safeSaveAsIni(config_, "conf/skey.conf");
    reloadConfig();
    updateMenuActions();
}

void SKeyEngine::setOutputMode(SKeyOutputMode mode) {
    config_.outputMode.setValue(mode);
    safeSaveAsIni(config_, "conf/skey.conf");
    updateMenuActions();
}

void SKeyEngine::setInputMethod(SKeyInputMethod method) {
    config_.inputMethod.setValue(method);
    safeSaveAsIni(config_, "conf/skey.conf");
    updateMenuActions();
}

void SKeyEngine::updateMenuActions() {
    auto im = config_.inputMethod.value();
    imTelex_.setChecked(im == SKeyInputMethod::Telex);
    imVni_.setChecked(im == SKeyInputMethod::VNI);
    imTelexW_.setChecked(im == SKeyInputMethod::TelexW);

    // Update parent label to show current selection
    if (im == SKeyInputMethod::VNI) {
        imAction_.setShortText(_("Input Method: VNI"));
    } else if (im == SKeyInputMethod::TelexW) {
        imAction_.setShortText(_("Input Method: Telex W"));
    } else {
        imAction_.setShortText(_("Input Method: Telex"));
    }

    auto om = config_.outputMode.value();
    omSurrounding_.setChecked(om == SKeyOutputMode::SurroundingText);
    omPreedit_.setChecked(om == SKeyOutputMode::Preedit);
    omSurroundingSlow_.setChecked(om == SKeyOutputMode::SurroundingTextSlow);

    if (om == SKeyOutputMode::Preedit) {
        omAction_.setShortText(_("Output Mode: Preedit"));
    } else if (om == SKeyOutputMode::SurroundingTextSlow) {
        omAction_.setShortText(_("Output Mode: Surrounding (slow)"));
    } else {
        omAction_.setShortText(_("Output Mode: Surrounding Text"));
    }
}

void SKeyEngine::saveAppMode(const std::string &app, SKeyOutputMode mode) {
    RawConfig cfg;
    readAsIni(cfg, "conf/skey-app-modes.conf");
    const char *val = (mode == SKeyOutputMode::SurroundingText)   ? "SurroundingText"
                      : (mode == SKeyOutputMode::Preedit)         ? "Preedit"
                                                               : "SurroundingTextSlow";
    cfg.setValueByPath(app, val);
    safeSaveAsIni(cfg, "conf/skey-app-modes.conf");
    SKEY_INFO() << "Saved app mode: " << app << " -> " << val;
}

SKeyOutputMode SKeyEngine::loadAppMode(const std::string &app) const {
    RawConfig cfg;
    readAsIni(cfg, "conf/skey-app-modes.conf");
    auto *val = cfg.valueByPath(app);
    if (val) {
        if (*val == "Preedit") return SKeyOutputMode::Preedit;
        if (*val == "SurroundingTextSlow") return SKeyOutputMode::SurroundingTextSlow;
        if (*val == "SurroundingText") return SKeyOutputMode::SurroundingText;
    }
    // Not found — return the configured default
    return config_.outputMode.value();
}

void SKeyEngine::reloadConfig() {
    readAsIni(config_, "conf/skey.conf");
    const char *modeStr = "Preedit";
    if (config_.outputMode.value() == SKeyOutputMode::SurroundingText)
        modeStr = "SurroundingText";
    else if (config_.outputMode.value() == SKeyOutputMode::SurroundingTextSlow)
        modeStr = "SurroundingTextSlow";
    SKEY_INFO() << "Config: outputMode=" << modeStr;
}

std::string SKeyEngine::subMode(const InputMethodEntry &entry,
                                InputContext &ic) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(ic);
    if (*config_.inputMethod == SKeyInputMethod::VNI) {
        return "VNI";
    }
    if (*config_.inputMethod == SKeyInputMethod::TelexW) {
        return "Telex W";
    }
    return "Telex";
}

// ---------------------------------------------------------------------------
// SKeyState
// ---------------------------------------------------------------------------

SKeyState::SKeyState(SKeyEngine *engine, InputContext *ic)
    : engine_(engine), ic_(ic) {
    auto &cfg = engine_->config();
    skey::InputMethod im = skey::InputMethod::Telex;
    if (*cfg.inputMethod == SKeyInputMethod::VNI) {
        im = skey::InputMethod::VNI;
    } else if (*cfg.inputMethod == SKeyInputMethod::TelexW) {
        im = skey::InputMethod::TelexW;
    }
    viet_.setMethod(im);
    viet_.setToneStyle(*cfg.tonePosition == TonePosition::Modern
                           ? skey::ToneStyle::Modern
                           : skey::ToneStyle::Traditional);
    viet_.setFreeMarking(*cfg.freeMarking);
}

SKeyOutputMode SKeyState::effectiveMode() const {
    if (hasAppModeOverride_)
        return appModeOverride_;
    return engine_->config().outputMode.value();
}

bool SKeyState::useSurroundingText() const {
    auto mode = effectiveMode();
    return mode == SKeyOutputMode::SurroundingText ||
           mode == SKeyOutputMode::SurroundingTextSlow;
}

bool SKeyState::useSlowMode() const {
    return effectiveMode() == SKeyOutputMode::SurroundingTextSlow;
}

bool SKeyState::canEditWithSurroundingText() const {
    return useSurroundingText() &&
           ic_->capabilityFlags().test(CapabilityFlag::SurroundingText);
}

bool SKeyState::useNativeSurroundingApi() const {
    return canEditWithSurroundingText();
}

bool SKeyState::useHiddenComposition() const {
    return false;
}

void SKeyState::activate() {
    // Re-sync input method from config (handles config changes at runtime)
    auto &cfg = engine_->config();
    skey::InputMethod im = skey::InputMethod::Telex;
    if (*cfg.inputMethod == SKeyInputMethod::VNI) {
        im = skey::InputMethod::VNI;
    } else if (*cfg.inputMethod == SKeyInputMethod::TelexW) {
        im = skey::InputMethod::TelexW;
    }
    viet_.setMethod(im);
    viet_.setToneStyle(*cfg.tonePosition == TonePosition::Modern
                           ? skey::ToneStyle::Modern
                           : skey::ToneStyle::Traditional);
    viet_.setFreeMarking(*cfg.freeMarking);

    viet_.reset();
    committedLen_ = 0;
    modeMenuActive_ = false;
    deferredCommitTimer_.reset();
    deferredCommitText_.clear();
    deferredPrefix_.clear();
    // Load per-app mode preference
    hasAppModeOverride_ = false;
    if (!ic_->program().empty()) {
        RawConfig cfg;
        readAsIni(cfg, "conf/skey-app-modes.conf");
        auto *val = cfg.valueByPath(ic_->program());
        if (val) {
            SKeyOutputMode savedMode = engine_->config().outputMode.value();
            if (*val == "Preedit") savedMode = SKeyOutputMode::Preedit;
            else if (*val == "SurroundingTextSlow") savedMode = SKeyOutputMode::SurroundingTextSlow;
            else if (*val == "SurroundingText") savedMode = SKeyOutputMode::SurroundingText;
            appModeOverride_ = savedMode;
            hasAppModeOverride_ = true;
        }
    }

    auto caps = ic_->capabilityFlags();
    auto mode = effectiveMode();
    auto configuredMode = engine_->config().outputMode.value();
    SKEY_DEBUG() << "Activated: mode="
                 << (mode == SKeyOutputMode::SurroundingText
                         ? "surrounding"
                         : (mode == SKeyOutputMode::Preedit
                                ? "preedit"
                                : "surrounding-slow"))
                 << " configured="
                 << (configuredMode == SKeyOutputMode::SurroundingText
                         ? "surrounding"
                         : (configuredMode == SKeyOutputMode::Preedit
                                ? "preedit"
                                : "surrounding-slow"))
                 << " surroundingCap="
                 << caps.test(CapabilityFlag::SurroundingText)
                 << " password=" << caps.test(CapabilityFlag::Password)
                 << " preeditCap=" << caps.test(CapabilityFlag::Preedit)
                 << " nativeSurrounding=" << useNativeSurroundingApi()
                 << " frontend=" << ic_->frontendName()
                 << " display=" << ic_->display()
                 << " app=" << ic_->program();
}

void SKeyState::deactivate() {
    forceFlushDeferredCommit();
    if (!viet_.getComposed().empty() && !useSurroundingText()) {
        commitBuffer();
    }
    viet_.reset();
    committedLen_ = 0;
    clearUI();
}

void SKeyState::reset() {
    if (hasDeferredCommitPending()) {
        SKEY_DEBUG() << "Reset: keeping deferred commit";
    }
    viet_.reset();
    if (!hasDeferredCommitPending()) {
        committedLen_ = 0;
    }
    clearUI();
}

void SKeyState::keyEvent(KeyEvent &keyEvent) {
    if (keyEvent.isRelease()) {
        return;
    }

    auto key = keyEvent.key();

    // ── Mode switch menu (activated by ` key) ──
    if (modeMenuActive_) {
        if (key.check(FcitxKey_Escape)) {
            SKEY_DEBUG() << "Menu: cancelled";
            dismissModeMenu();
            keyEvent.filterAndAccept();
            return;
        }
        auto sym = key.sym();
        int choice = 0;
        if (sym == FcitxKey_1 || sym == FcitxKey_KP_1) choice = 1;
        else if (sym == FcitxKey_2 || sym == FcitxKey_KP_2) choice = 2;
        else if (sym == FcitxKey_3 || sym == FcitxKey_KP_3) choice = 3;

        if (choice > 0) {
            SKeyOutputMode newMode;
            switch (choice) {
            case 1: newMode = SKeyOutputMode::SurroundingText; break;
            case 2: newMode = SKeyOutputMode::Preedit; break;
            case 3: newMode = SKeyOutputMode::SurroundingTextSlow; break;
            default: dismissModeMenu(); keyEvent.filterAndAccept(); return;
            }
            appModeOverride_ = newMode;
            hasAppModeOverride_ = true;
            engine_->saveAppMode(ic_->program(), newMode);
            SKEY_INFO() << "Mode switched to "
                        << (newMode == SKeyOutputMode::SurroundingText
                                ? "SurroundingText"
                                : (newMode == SKeyOutputMode::Preedit
                                       ? "Preedit"
                                       : "SurroundingTextSlow"));
            dismissModeMenu();
            keyEvent.filterAndAccept();
            return;
        }
        // Any other key: dismiss menu, pass key through to app
        SKEY_DEBUG() << "Menu: dismissed by key";
        dismissModeMenu();
        // Not filtered — key passes through
        return;
    }

    // ── Backtick (`) shows mode switch menu when not composing ──
    if (key.check(FcitxKey_grave) && viet_.getRawInput().empty() &&
        !hasDeferredCommitPending()) {
        showModeMenu();
        keyEvent.filterAndAccept();
        return;
    }

    auto sym = key.sym();
    bool pendingCanMerge = false;
    if (hasDeferredCommitPending()) {
        char pendingCh = 0;
        bool pendingIsDigit = false;
        if (sym >= FcitxKey_exclam && sym <= FcitxKey_asciitilde) {
            pendingCh = static_cast<char>(sym);
            pendingIsDigit = pendingCh >= '0' && pendingCh <= '9';
            pendingCanMerge = (pendingCh >= 'a' && pendingCh <= 'z') ||
                              (pendingCh >= 'A' && pendingCh <= 'Z') ||
                              (engine_->config().inputMethod.value() ==
                                   SKeyInputMethod::VNI &&
                               pendingIsDigit && !viet_.getRawInput().empty());
        }
        if (!pendingCanMerge) {
            flushDeferredCommit();
            // If flush was deferred (BS not processed yet), we still
            // need to let the timer handle it. Pass key through.
        }
    }

    // Pass through modifier keys (except Shift and CapsLock)
    if (key.states() &
        ~KeyStates({KeyState::Shift, KeyState::CapsLock})) {
        if (!viet_.getRawInput().empty()) {
            if (useSurroundingText()) {
                forceFlushDeferredCommit();
            } else {
                commitBuffer();
                clearUI();
            }
            viet_.reset();
            committedLen_ = 0;
        }
        return;
    }

    // Handle Backspace while composing
    if (key.check(FcitxKey_BackSpace) && !viet_.getRawInput().empty()) {
        if (useSurroundingText()) {
            surroundingBackspace();
        } else {
            viet_.backspace();
            if (viet_.getRawInput().empty()) {
                clearUI();
            } else {
                updatePreedit();
            }
        }
        keyEvent.filterAndAccept();
        return;
    }

    // Handle Escape
    if (key.check(FcitxKey_Escape) && !viet_.getRawInput().empty()) {
        viet_.reset();
        committedLen_ = 0;
        if (!useSurroundingText()) {
            clearUI();
        }
        keyEvent.filterAndAccept();
        return;
    }

    // Handle Enter
    if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
        if (!viet_.getRawInput().empty()) {
            if (useSurroundingText()) {
                forceFlushDeferredCommit();
            } else {
                commitBuffer();
                clearUI();
            }
            viet_.reset();
            committedLen_ = 0;
        }
        return;
    }

    // Handle Space
    if (key.check(FcitxKey_space)) {
        if (!viet_.getRawInput().empty()) {
            if (useSurroundingText()) {
                bool hadDeferred = hasDeferredCommitPending();
                if (hadDeferred) {
                    pendingFlushSuffix_ += " ";
                }
                forceFlushDeferredCommit();
                if (!hadDeferred) {
                    ic_->commitString(" ");
                }
                keyEvent.filterAndAccept();
            } else {
                commitBuffer();
                clearUI();
            }
            viet_.reset();
            committedLen_ = 0;
        }
        return;
    }

    // Handle Tab
    if (key.check(FcitxKey_Tab)) {
        if (!viet_.getRawInput().empty()) {
            if (useSurroundingText()) {
                forceFlushDeferredCommit();
            } else {
                commitBuffer();
                clearUI();
            }
            viet_.reset();
            committedLen_ = 0;
        }
        return;
    }

    // Process printable ASCII
    if (sym >= FcitxKey_exclam && sym <= FcitxKey_asciitilde) {
        char ch = static_cast<char>(sym);

        bool isLetter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        bool isDigit = (ch >= '0' && ch <= '9');
        bool isVNIModifier =
            (engine_->config().inputMethod.value() == SKeyInputMethod::VNI) &&
            isDigit && !viet_.getRawInput().empty();

        if (isLetter || isVNIModifier) {
            std::string oldComposed = viet_.getComposed();

            auto result = viet_.processKey(ch);

            if (result == skey::ProcessResult::Committed) {
                std::string committed = viet_.getCommitted();
                viet_.clearCommitted();
                std::string newComposed = viet_.getComposed();

                if (useSurroundingText()) {
                    // In surrounding text mode, oldComposed is already on screen.
                    // Replace it with committed + newComposed.
                    std::string fullNew = committed + newComposed;
                    if (!fullNew.empty()) {
                        surroundingCommit(oldComposed, fullNew);
                        committedLen_ = static_cast<int>(
                            utf8::length(fullNew));
                    } else {
                        // Both committed and newComposed are empty — just
                        // delete old text from screen
                        surroundingCommit(oldComposed, "");
                        committedLen_ = 0;
                    }
                } else {
                    if (!committed.empty()) {
                        ic_->commitString(committed);
                    }
                    committedLen_ = 0;
                    if (!newComposed.empty()) {
                        updatePreedit();
                    } else {
                        clearUI();
                    }
                }
                keyEvent.filterAndAccept();
                return;
            }

            std::string newComposed = viet_.getComposed();
            SKEY_DEBUG() << "Key '" << ch << "': old='" << oldComposed
                         << "' new='" << newComposed << "' len="
                         << committedLen_;

            if (!newComposed.empty()) {
                if (useSurroundingText()) {
                    surroundingCommit(oldComposed, newComposed);
                } else {
                    updatePreedit();
                }
            } else {
                if (!useSurroundingText()) {
                    clearUI();
                }
                committedLen_ = 0;
            }
            keyEvent.filterAndAccept();
            return;
        }

        // Non-letter: finalize
        if (!viet_.getRawInput().empty()) {
            if (useSurroundingText()) {
                bool hadDeferred = hasDeferredCommitPending();
                if (hadDeferred) {
                    pendingFlushSuffix_ += std::string(1, ch);
                }
                forceFlushDeferredCommit();
                if (!hadDeferred) {
                    ic_->commitString(std::string(1, ch));
                }
                keyEvent.filterAndAccept();
            } else {
                commitBuffer();
                clearUI();
            }
            viet_.reset();
            committedLen_ = 0;
        }
        return;
    }

    // Any other key: finalize
    if (!viet_.getRawInput().empty()) {
        if (useSurroundingText()) {
            forceFlushDeferredCommit();
        } else {
            commitBuffer();
            clearUI();
        }
        viet_.reset();
        committedLen_ = 0;
    }
}

void SKeyState::commitBuffer() {
    std::string text = viet_.getComposed();
    SKEY_DEBUG() << "Commit: '" << text << "'";
    if (!text.empty()) {
        ic_->commitString(text);
    }
    viet_.reset();
}

bool SKeyState::hasDeferredCommitPending() const {
    return !deferredCommitText_.empty();
}

void SKeyState::scheduleDeferredCommit(const std::string &text,
                                       const std::string &stablePrefix) {
    deferredCommitTimer_.reset();
    deferredCommitText_ = text;
    deferredPrefix_ = stablePrefix;
    pendingFlushSuffix_.clear();

    // Delay after BackSpace to ensure the app has processed the BS key
    // events before we commit new text via commitString (different D-Bus
    // channel). No preedit is used (no underline).
    //
    // 80ms is required for Electron apps (Tabby, Antigravity) where BS
    // processing takes ~60-70ms through D-Bus → main process → IPC →
    // renderer → DOM pipeline.
    //
    // During fast typing this delay is invisible: each keystroke resets
    // the timer via the deferred update path in surroundingCommit(), so
    // the commit only happens when the user pauses (natural word boundary).
    uint64_t delayUsec = 80000;  // 80ms

    SKEY_DEBUG() << "Surr deferred: schedule '" << text << "' in "
                 << (delayUsec / 1000) << "ms";
    deferredCommitTimer_ = engine_->instance()->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        now(CLOCK_MONOTONIC) + delayUsec,
        0,
        [this](EventSourceTime *, uint64_t) {
            SKEY_DEBUG() << "Surr deferred: timer commit '"
                         << deferredCommitText_ << "'";
            std::string toCommit = deferredCommitText_ + pendingFlushSuffix_;
            deferredCommitText_.clear();
            deferredPrefix_.clear();
            deferredBsSentAt_ = 0;
            pendingFlushSuffix_.clear();
            deferredCommitTimer_.reset();
            ic_->commitString(toCommit);
            return true;
        });
}

void SKeyState::flushDeferredCommit() {
    if (!hasDeferredCommitPending()) {
        deferredCommitTimer_.reset();
        return;
    }

    // Enforce minimum delay between BackSpace and commit.
    // If BS was sent recently, the app might not have processed it yet.
    uint64_t minGapUsec = 80000;  // 80ms — match scheduleDeferredCommit
    if (deferredBsSentAt_ > 0) {
        uint64_t nowUs = now(CLOCK_MONOTONIC);
        uint64_t elapsed = nowUs - deferredBsSentAt_;
        if (elapsed < minGapUsec) {
            // Not safe to commit yet — reschedule for the remaining time.
            uint64_t remaining = minGapUsec - elapsed;
            SKEY_DEBUG() << "Surr deferred: flush delayed "
                         << (remaining / 1000) << "ms (BS not processed)";
            deferredCommitTimer_.reset();
            deferredCommitTimer_ = engine_->instance()->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC,
                nowUs + remaining,
                0,
                [this](EventSourceTime *, uint64_t) {
                    SKEY_DEBUG() << "Surr deferred: delayed flush commit '"
                                 << deferredCommitText_ << "'";
                    std::string toCommit = deferredCommitText_ + pendingFlushSuffix_;
                    deferredCommitText_.clear();
                    deferredPrefix_.clear();
                    deferredBsSentAt_ = 0;
                    pendingFlushSuffix_.clear();
                    deferredCommitTimer_.reset();
                    ic_->commitString(toCommit);
                    return true;
                });
            return;
        }
    }

    // Safe to commit now — BS has been processed.
    SKEY_DEBUG() << "Surr deferred: flush commit '" << deferredCommitText_ << "'";
    std::string toCommit = deferredCommitText_ + pendingFlushSuffix_;
    deferredCommitText_.clear();
    deferredPrefix_.clear();
    deferredBsSentAt_ = 0;
    pendingFlushSuffix_.clear();
    deferredCommitTimer_.reset();
    ic_->commitString(toCommit);
}

void SKeyState::forceFlushDeferredCommit() {
    if (!hasDeferredCommitPending()) {
        deferredCommitTimer_.reset();
        return;
    }
    // Force-commit immediately — don't wait for BS timing.
    // Used at word boundaries (space/enter/punctuation) to prevent
    // stale deferred commits from corrupting new word composition.
    SKEY_DEBUG() << "Surr deferred: force flush commit '" << deferredCommitText_ << "'";
    std::string toCommit = deferredCommitText_ + pendingFlushSuffix_;
    deferredCommitText_.clear();
    deferredPrefix_.clear();
    deferredBsSentAt_ = 0;
    pendingFlushSuffix_.clear();
    deferredCommitTimer_.reset();
    ic_->commitString(toCommit);
}

void SKeyState::surroundingCommit(const std::string &oldComposed,
                                  const std::string &newComposed) {
    if (newComposed.empty()) return;

    int newLen = static_cast<int>(utf8::length(newComposed));

    if (hasDeferredCommitPending()) {
        SKEY_DEBUG() << "Surr deferred: update pending '" << deferredCommitText_
                     << "' -> '" << newComposed << "'";
        committedLen_ = newLen;
        // Preserve the original BS timestamp — the BackSpace from the initial
        // replace still hasn't been processed by the app.
        uint64_t savedBsTime = deferredBsSentAt_;
        if (!deferredPrefix_.empty() &&
            newComposed.compare(0, deferredPrefix_.size(), deferredPrefix_) == 0) {
            scheduleDeferredCommit(newComposed.substr(deferredPrefix_.size()),
                                   deferredPrefix_);
        } else {
            scheduleDeferredCommit(newComposed);
        }
        deferredBsSentAt_ = savedBsTime;
        return;
    }

    bool isSimpleAppend =
        !oldComposed.empty() && newComposed.size() > oldComposed.size() &&
        newComposed.compare(0, oldComposed.size(), oldComposed) == 0;

    if (oldComposed.empty()) {
        SKEY_DEBUG() << "Surr: first '" << newComposed << "'";
        ic_->commitString(newComposed);
        committedLen_ = newLen;
    } else if (isSimpleAppend) {
        std::string appended = newComposed.substr(oldComposed.size());
        SKEY_DEBUG() << "Surr: append '" << appended << "'";
        ic_->commitString(appended);
        committedLen_ = newLen;
    } else {
        size_t commonPrefix = commonUtf8PrefixBytes(oldComposed, newComposed);
        std::string deletedPart = oldComposed.substr(commonPrefix);
        std::string addedPart = newComposed.substr(commonPrefix);
        std::string stablePrefix = newComposed.substr(0, commonPrefix);
        int deleteLen = static_cast<int>(utf8::length(deletedPart));

        SKEY_DEBUG() << "Surr: replace '" << oldComposed << "' -> '"
                     << newComposed << "' (delete suffix x"
                     << deleteLen << ")";
        if (deleteLen > 0) {
            // Helper: delete via BackSpace forwarding, then commit.
            // XIM apps process forwarded keys synchronously (same X11 connection),
            // so we can commit immediately. D-Bus apps need a deferred commit
            // because the key goes through multiple async IPC hops.
            auto deleteViaBackspace = [&]() {
                bool needDelay = useSlowMode();
                SKEY_DEBUG() << "Surr: BS x" << deleteLen
                             << (needDelay ? " (slow mode, deferred)" : " (direct)");
                deferredBsSentAt_ = now(CLOCK_MONOTONIC);
                for (int i = 0; i < deleteLen; ++i) {
                    ic_->forwardKey(Key(FcitxKey_BackSpace));
                }
                committedLen_ = newLen;
                if (!addedPart.empty()) {
                    if (needDelay) {
                        scheduleDeferredCommit(addedPart, stablePrefix);
                    } else {
                        SKEY_DEBUG() << "Surr: direct commit after BS '" << addedPart << "'";
                        ic_->commitString(addedPart);
                    }
                }
            };

            if (useNativeSurroundingApi()) {
                const auto &surrounding = ic_->surroundingText();
                if (!surrounding.isValid() ||
                    surrounding.cursor() < static_cast<unsigned int>(deleteLen)) {
                    SKEY_DEBUG() << "Surr: native surrounding not ready";
                    deleteViaBackspace();
                } else {
                    ic_->deleteSurroundingText(-deleteLen, deleteLen);
                    if (surrounding.isValid()) {
                        ic_->surroundingText().deleteText(-deleteLen, deleteLen);
                    }
                    committedLen_ = newLen;
                    if (!addedPart.empty()) {
                        SKEY_DEBUG() << "Surr: direct commit '" << addedPart << "'";
                        ic_->commitString(addedPart);
                    }
                }
            } else {
                SKEY_DEBUG() << "Surr: client has no surrounding text capability";
                deleteViaBackspace();
            }
        } else {
            // deleteLen == 0: no deletion needed, only add new suffix if any
            if (!addedPart.empty()) {
                ic_->commitString(addedPart);
            }
            committedLen_ = newLen;
        }
    }
}

void SKeyState::surroundingBackspace() {
    if (committedLen_ <= 0) return;

    SKEY_DEBUG() << "SurrBS: delete surrounding 1, len=" << committedLen_
                 << " -> " << (committedLen_ - 1);

    if (useNativeSurroundingApi()) {
        ic_->deleteSurroundingText(-1, 1);
        if (ic_->surroundingText().isValid()) {
            ic_->surroundingText().deleteText(-1, 1);
        }
    } else {
        SKEY_DEBUG() << "SurrBS: client has no surrounding text capability, fallback BS";
        ic_->forwardKey(Key(FcitxKey_BackSpace));
    }
    committedLen_--;

    // Reset engine so next keystroke starts fresh composition.
    // The old committed chars (before cursor) stay in the app untouched.
    viet_.reset();
}

void SKeyState::updateDeferredPreedit() {
    // During deferred commit (slow surrounding text mode), show the pending
    // text as preedit so the user gets immediate visual feedback while we
    // wait for BackSpace to be processed before the actual commit.
    Text preedit;
    if (!deferredCommitText_.empty()) {
        preedit.append(deferredCommitText_, TextFormatFlag::Underline);
        preedit.setCursor(deferredCommitText_.size());
    }
    if (ic_->capabilityFlags().test(CapabilityFlag::Preedit)) {
        ic_->inputPanel().setClientPreedit(preedit);
    } else {
        ic_->inputPanel().setPreedit(preedit);
    }
    ic_->updatePreedit();
    ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void SKeyState::forwardUtf8AsKeys(const std::string &text) {
    // Forward each UTF-8 character as a key event using Unicode keysyms.
    // This ensures BS + replacement chars all go through the same
    // key event handler in the app, preserving ordering.
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        uint8_t lead = static_cast<uint8_t>(text[i]);
        int seqLen;
        if (lead < 0x80)       { cp = lead;          seqLen = 1; }
        else if (lead < 0xC0)  { i++; continue; /* continuation byte, skip */ }
        else if (lead < 0xE0)  { cp = lead & 0x1F;   seqLen = 2; }
        else if (lead < 0xF0)  { cp = lead & 0x0F;   seqLen = 3; }
        else                   { cp = lead & 0x07;   seqLen = 4; }
        for (int j = 1; j < seqLen && i + j < text.size(); j++) {
            cp = (cp << 6) | (static_cast<uint8_t>(text[i + j]) & 0x3F);
        }
        i += seqLen;
        // Unicode keysym range: 0x01000000 + codepoint
        ic_->forwardKey(Key(static_cast<KeySym>(0x01000000 | cp)));
    }
    SKEY_DEBUG() << "Surr: forwarded '" << text << "' as "
                 << utf8::length(text) << " key events";
}

void SKeyState::updatePreedit() {
    Text clientPreedit;
    std::string composed = viet_.getComposed();
    if (!composed.empty()) {
        clientPreedit.append(composed, TextFormatFlag::Underline);
        clientPreedit.setCursor(composed.size());
    }

    if (ic_->capabilityFlags().test(CapabilityFlag::Preedit)) {
        ic_->inputPanel().setClientPreedit(clientPreedit);
    } else {
        ic_->inputPanel().setPreedit(clientPreedit);
    }
    ic_->updatePreedit();
    ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void SKeyState::clearUI() {
    ic_->inputPanel().reset();
    ic_->updatePreedit();
    ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    modeMenuActive_ = false;
}

void SKeyState::showModeMenu() {
    modeMenuActive_ = true;
    auto mode = effectiveMode();

    // Build candidate list for dropdown menu
    auto candList = std::make_unique<CommonCandidateList>();
    candList->setPageSize(3);
    candList->setLayoutHint(CandidateLayoutHint::Vertical);

    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "1. Surrounding Text",
        SKeyOutputMode::SurroundingText));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "2. Preedit", SKeyOutputMode::Preedit));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "3. Surrounding Text (slow)",
        SKeyOutputMode::SurroundingTextSlow));

    // Highlight the currently active mode via cursor index
    int cursorIdx = (mode == SKeyOutputMode::SurroundingText)       ? 0
                    : (mode == SKeyOutputMode::Preedit)             ? 1
                    : (mode == SKeyOutputMode::SurroundingTextSlow) ? 2
                                                                     : 0;
    candList->setGlobalCursorIndex(cursorIdx);

    ic_->inputPanel().setCandidateList(std::move(candList));
    ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    SKEY_DEBUG() << "Menu: mode switch dropdown shown (cursor=" << cursorIdx << ")";
}

void SKeyState::dismissModeMenu() {
    modeMenuActive_ = false;
    ic_->inputPanel().setCandidateList(nullptr);
    clearUI();
}

} // namespace fcitx
