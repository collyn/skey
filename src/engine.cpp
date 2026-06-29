#include "engine.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>

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
    SKEY_INFO() << "SKey Vietnamese Input Method loaded";
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
    auto *state = event.inputContext()->propertyFor(&factory_);
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
}

void SKeyEngine::reloadConfig() {
    readAsIni(config_, "conf/skey.conf");
    SKEY_INFO() << "Config: outputMode="
                << (config_.outputMode.value() == SKeyOutputMode::SurroundingText
                        ? "SurroundingText"
                        : "Preedit");
}

std::string SKeyEngine::subMode(const InputMethodEntry &entry,
                                InputContext &ic) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(ic);
    if (*config_.inputMethod == SKeyInputMethod::VNI) {
        return "VNI";
    }
    return "Telex";
}

// ---------------------------------------------------------------------------
// SKeyState
// ---------------------------------------------------------------------------

SKeyState::SKeyState(SKeyEngine *engine, InputContext *ic)
    : engine_(engine), ic_(ic) {
    auto &cfg = engine_->config();
    viet_.setMethod(*cfg.inputMethod == SKeyInputMethod::Telex
                        ? skey::InputMethod::Telex
                        : skey::InputMethod::VNI);
    viet_.setToneStyle(*cfg.tonePosition == TonePosition::Modern
                           ? skey::ToneStyle::Modern
                           : skey::ToneStyle::Traditional);
    viet_.setFreeMarking(*cfg.freeMarking);
}

bool SKeyState::useSurroundingText() const {
    return engine_->config().outputMode.value() ==
           SKeyOutputMode::SurroundingText;
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
    viet_.reset();
    committedLen_ = 0;
    deferredCommitTimer_.reset();
    deferredCommitText_.clear();
    deferredPrefix_.clear();
    SKEY_DEBUG() << "Activated: mode="
                 << (useSurroundingText()
                         ? "surrounding"
                         : "preedit")
                 << " configured="
                 << (engine_->config().outputMode.value() ==
                             SKeyOutputMode::SurroundingText
                         ? "surrounding"
                         : "preedit")
                 << " surroundingCap="
                 << ic_->capabilityFlags().test(CapabilityFlag::SurroundingText)
                 << " nativeSurrounding=" << useNativeSurroundingApi()
                 << " app=" << ic_->program();
}

void SKeyState::deactivate() {
    flushDeferredCommit();
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
        }
    }

    // Pass through modifier keys (except Shift and CapsLock)
    if (key.states() &
        ~KeyStates({KeyState::Shift, KeyState::CapsLock})) {
        if (!viet_.getRawInput().empty()) {
            if (useSurroundingText()) {
                flushDeferredCommit();
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
                flushDeferredCommit();
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
                flushDeferredCommit();
                ic_->commitString(" ");
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
                flushDeferredCommit();
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
                ic_->commitString(committed);
                viet_.clearCommitted();
                committedLen_ = 0;
                oldComposed.clear();
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
                flushDeferredCommit();
                ic_->commitString(std::string(1, ch));
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
            flushDeferredCommit();
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
    return deferredCommitTimer_ && !deferredCommitText_.empty();
}

void SKeyState::scheduleDeferredCommit(const std::string &text,
                                       const std::string &stablePrefix) {
    deferredCommitTimer_.reset();
    deferredCommitText_ = text;
    deferredPrefix_ = stablePrefix;
    uint64_t delayUsec = 80000;
    SKEY_DEBUG() << "Surr deferred: schedule commit '" << text << "' in "
                 << (delayUsec / 1000) << "ms";
    deferredCommitTimer_ = engine_->instance()->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        now(CLOCK_MONOTONIC) + delayUsec,
        0,
        [this](EventSourceTime *, uint64_t) {
            SKEY_DEBUG() << "Surr deferred: timer commit '" << deferredCommitText_ << "'";
            ic_->commitString(deferredCommitText_);
            deferredCommitText_.clear();
            deferredPrefix_.clear();
            deferredCommitTimer_.reset();
            return true;
        });
}

void SKeyState::flushDeferredCommit() {
    if (hasDeferredCommitPending()) {
        SKEY_DEBUG() << "Surr deferred: flush commit '" << deferredCommitText_ << "'";
        ic_->commitString(deferredCommitText_);
    }
    deferredCommitText_.clear();
    deferredPrefix_.clear();
    deferredCommitTimer_.reset();
}

void SKeyState::surroundingCommit(const std::string &oldComposed,
                                  const std::string &newComposed) {
    if (newComposed.empty()) return;

    int newLen = static_cast<int>(utf8::length(newComposed));

    if (hasDeferredCommitPending()) {
        SKEY_DEBUG() << "Surr deferred: update pending '" << deferredCommitText_
                     << "' -> '" << newComposed << "'";
        committedLen_ = newLen;
        if (!deferredPrefix_.empty() &&
            newComposed.compare(0, deferredPrefix_.size(), deferredPrefix_) == 0) {
            scheduleDeferredCommit(newComposed.substr(deferredPrefix_.size()),
                                   deferredPrefix_);
        } else {
            scheduleDeferredCommit(newComposed);
        }
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
            if (useNativeSurroundingApi()) {
                const auto &surrounding = ic_->surroundingText();
                if (!surrounding.isValid() ||
                    surrounding.cursor() < static_cast<unsigned int>(deleteLen)) {
                    SKEY_DEBUG() << "Surr: native surrounding not ready, fallback BS";
                    for (int i = 0; i < deleteLen; ++i) {
                        ic_->forwardKey(Key(FcitxKey_BackSpace));
                    }
                    committedLen_ = newLen;
                    scheduleDeferredCommit(addedPart, stablePrefix);
                } else {
                    ic_->deleteSurroundingText(-deleteLen, deleteLen);
                    if (surrounding.isValid()) {
                        ic_->surroundingText().deleteText(-deleteLen, deleteLen);
                    }
                    committedLen_ = newLen;
                    scheduleDeferredCommit(addedPart, stablePrefix);
                }
            } else {
                SKEY_DEBUG() << "Surr: client has no surrounding text capability, fallback BS";
                for (int i = 0; i < deleteLen; ++i) {
                    ic_->forwardKey(Key(FcitxKey_BackSpace));
                }
                committedLen_ = newLen;
                scheduleDeferredCommit(addedPart, stablePrefix);
            }
        } else {
            ic_->commitString(newComposed);
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
}

} // namespace fcitx
