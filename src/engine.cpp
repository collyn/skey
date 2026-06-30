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
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <vector>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

static std::string outputModeName(SKeyOutputMode mode) {
    switch (mode) {
    case SKeyOutputMode::SurroundingText:
        return "SurroundingText";
    case SKeyOutputMode::Preedit:
        return "Preedit";
    case SKeyOutputMode::SurroundingTextSlow:
        return "SurroundingTextSlow";
    case SKeyOutputMode::Uinput:
        return "Uinput";
    }
    return "SurroundingText";
}

static constexpr size_t maxBufferedUinputKeys = 32;
static constexpr uint64_t uinputCommitMinDelayUsec = 10000;  // 10ms floor
static constexpr uint64_t uinputCommitDefaultDelayUsec = 10000; // 10ms fallback


static std::string skeySocketPath(const char *suffix) {
    struct passwd pwd {};
    struct passwd *result = nullptr;
    long bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufSize < 0) {
        bufSize = 16384;
    }
    std::vector<char> buf(static_cast<size_t>(bufSize));
    std::string username = "unknown";
    if (getpwuid_r(getuid(), &pwd, buf.data(), buf.size(), &result) == 0 &&
        result) {
        username = result->pw_name;
    }

    std::string path = std::string("skeysocket-") + username + "-" + suffix;
    constexpr size_t maxAbstractSocketName =
        sizeof(((sockaddr_un *)0)->sun_path) - 1;
    if (path.size() > maxAbstractSocketName) {
        path.resize(maxAbstractSocketName);
    }
    return path;
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
        SKEY_INFO() << "Mode switched to " << outputModeName(mode_);
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
    omUinput_.setShortText(_("Uinput"));
    omUinput_.setCheckable(true);
    omUinput_.registerAction("skey-om-uinput", &uiManager);

    omMenu_.addAction(&omSurrounding_);
    omMenu_.addAction(&omPreedit_);
    omMenu_.addAction(&omSurroundingSlow_);
    omMenu_.addAction(&omUinput_);

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
    omUinput_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        FCITX_UNUSED(ic);
        setOutputMode(SKeyOutputMode::Uinput);
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
    omUinput_.setChecked(om == SKeyOutputMode::Uinput);

    if (om == SKeyOutputMode::Preedit) {
        omAction_.setShortText(_("Output Mode: Preedit"));
    } else if (om == SKeyOutputMode::SurroundingTextSlow) {
        omAction_.setShortText(_("Output Mode: Surrounding (slow)"));
    } else if (om == SKeyOutputMode::Uinput) {
        omAction_.setShortText(_("Output Mode: Uinput"));
    } else {
        omAction_.setShortText(_("Output Mode: Surrounding Text"));
    }
}

void SKeyEngine::saveAppMode(const std::string &app, SKeyOutputMode mode) {
    RawConfig cfg;
    readAsIni(cfg, "conf/skey-app-modes.conf");
    std::string val = outputModeName(mode);
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
        if (*val == "Uinput") return SKeyOutputMode::Uinput;
        if (*val == "SurroundingText") return SKeyOutputMode::SurroundingText;
    }
    // Not found — return the configured default
    return config_.outputMode.value();
}

void SKeyEngine::reloadConfig() {
    readAsIni(config_, "conf/skey.conf");
    std::string modeStr = outputModeName(config_.outputMode.value());
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
           mode == SKeyOutputMode::SurroundingTextSlow ||
           mode == SKeyOutputMode::Uinput;
}

bool SKeyState::useSlowMode() const {
    return effectiveMode() == SKeyOutputMode::SurroundingTextSlow;
}

bool SKeyState::useUinputMode() const {
    return effectiveMode() == SKeyOutputMode::Uinput;
}

bool SKeyState::canEditWithSurroundingText() const {
    return useSurroundingText() &&
           ic_->capabilityFlags().test(CapabilityFlag::SurroundingText);
}

bool SKeyState::useNativeSurroundingApi() const {
    return !useUinputMode() && canEditWithSurroundingText();
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
            else if (*val == "Uinput") savedMode = SKeyOutputMode::Uinput;
            else if (*val == "SurroundingText") savedMode = SKeyOutputMode::SurroundingText;
            appModeOverride_ = savedMode;
            hasAppModeOverride_ = true;
        }
    }

    auto caps = ic_->capabilityFlags();
    auto mode = effectiveMode();
    auto configuredMode = engine_->config().outputMode.value();
    SKEY_DEBUG() << "Activated: mode="
                 << outputModeName(mode)
                 << " configured="
                 << outputModeName(configuredMode)
                 << " surroundingCap="
                 << caps.test(CapabilityFlag::SurroundingText)
                 << " password=" << caps.test(CapabilityFlag::Password)
                 << " preeditCap=" << caps.test(CapabilityFlag::Preedit)
                 << " nativeSurrounding=" << useNativeSurroundingApi()
                 << " frontend=" << ic_->frontendName()
                 << " display=" << ic_->display()
                 << " app=" << ic_->program();
}

bool SKeyState::connectUinputServer() {
    if (uinputClientFd_ >= 0) {
        return true;
    }

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        SKEY_DEBUG() << "Uinput: socket failed: " << strerror(errno);
        return false;
    }

    std::string path = skeySocketPath("kb_socket");
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    memcpy(&addr.sun_path[1], path.c_str(), path.size());
    socklen_t len = offsetof(sockaddr_un, sun_path) + path.size() + 1;
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), len) == 0) {
        uinputClientFd_ = fd;
        SKEY_DEBUG() << "Uinput: connected";
        return true;
    }

    SKEY_DEBUG() << "Uinput: connect failed: " << strerror(errno);
    close(fd);
    return false;
}

void SKeyState::sendBackspaceUinput(int count) {
    if (count <= 0) {
        return;
    }
    if (!connectUinputServer()) {
        SKEY_DEBUG() << "Uinput: cannot send BS, server unavailable";
        return;
    }

    // Send BS-only to the server.  The server injects N backspace key
    // events through the kernel.  Text is committed separately via
    // D-Bus commitString after the BS events have been processed.
    int32_t count32 = count;
    uint32_t textLen = 0;
    std::vector<char> msg(sizeof(int32_t) + sizeof(uint32_t));
    memcpy(msg.data(), &count32, sizeof(count32));
    memcpy(msg.data() + sizeof(count32), &textLen, sizeof(textLen));

    bsSentAt_ = now(CLOCK_MONOTONIC);
    ssize_t n = send(uinputClientFd_, msg.data(), msg.size(), MSG_NOSIGNAL);
    if (n < 0) {
        SKEY_DEBUG() << "Uinput: send failed: " << strerror(errno);
        close(uinputClientFd_);
        uinputClientFd_ = -1;
        if (connectUinputServer()) {
            send(uinputClientFd_, msg.data(), msg.size(), MSG_NOSIGNAL);
        }
    }
    SKEY_DEBUG() << "Uinput: sent BS=" << count;
}

bool SKeyState::handlePendingUinputBackspace(KeyEvent &keyEvent) {
    if (!uinputDeleting_) {
        return false;
    }

    // While waiting for BS, buffer ANY non-BS keys (including user-typed
    // space, letters, etc.) so they don't reach the app prematurely.
    if (!keyEvent.key().check(FcitxKey_BackSpace) ||
        expectedUinputBackspaces_ == 0) {
        auto sym = keyEvent.key().sym();
        std::string keyUtf8 = Key::keySymToUTF8(sym);
        if (!keyUtf8.empty() &&
            bufferedUinputKeys_.size() < maxBufferedUinputKeys) {
            SKEY_DEBUG() << "Uinput: buffer key '" << keyUtf8
                         << "' while deleting";
            bufferedUinputKeys_.push_back(sym);
        }
        keyEvent.filterAndAccept();
        return true;
    }

    // Count BS events.  ALL of them pass through to the app (no sentinel).
    ++seenUinputBackspaces_;
    SKEY_DEBUG() << "Uinput: pass BS " << seenUinputBackspaces_ << "/"
                 << expectedUinputBackspaces_;

    if (seenUinputBackspaces_ >= expectedUinputBackspaces_) {
        // All BS events have passed through.  Schedule commitString
        // via a timer to let the app finish processing the deletions.
        expectedUinputBackspaces_ = 0;
        seenUinputBackspaces_ = 0;

        std::string commitText = pendingUinputCommit_;
        pendingUinputCommit_.clear();

        // Adaptive delay: 3× round-trip.
        uint64_t elapsed = now(CLOCK_MONOTONIC) - bsSentAt_;
        uint64_t delay = std::max(elapsed * 3, uinputCommitMinDelayUsec);
        lastBsRoundTrip_ = elapsed;
        SKEY_DEBUG() << "Uinput: all BS passed, round-trip " << (elapsed / 1000)
                     << "ms, commit delay " << (delay / 1000) << "ms";

        uinputCommitTimer_ = engine_->instance()->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC,
            now(CLOCK_MONOTONIC) + delay,
            0,
            [this, commitText](EventSourceTime *, uint64_t) {
                uinputCommitTimer_.reset();
                uinputDeleting_ = false;
                if (!commitText.empty()) {
                    SKEY_DEBUG() << "Uinput: timer commit '" << commitText << "'";
                    ic_->commitString(commitText);
                }
                if (!bufferedUinputKeys_.empty()) {
                    replayBufferedUinputKeys();
                }
                return true;
            });
    }
    return true;  // let BS reach the app (not consumed)
}

void SKeyState::replayBufferedUinputKeys() {
    if (bufferedUinputKeys_.empty()) {
        return;
    }

    auto keys = std::move(bufferedUinputKeys_);
    bufferedUinputKeys_.clear();
    SKEY_DEBUG() << "Uinput: replay " << keys.size() << " buffered key(s)";

    for (size_t i = 0; i < keys.size(); ++i) {
        auto sym = keys[i];
        std::string keyUtf8 = Key::keySymToUTF8(sym);
        if (keyUtf8.empty()) {
            continue;
        }
        if (sym < FcitxKey_exclam || sym > FcitxKey_asciitilde) {
            ic_->commitString(keyUtf8);
            continue;
        }

        char ch = static_cast<char>(sym);
        bool isLetter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        bool isDigit = (ch >= '0' && ch <= '9');
        bool isVNIModifier =
            (engine_->config().inputMethod.value() == SKeyInputMethod::VNI) &&
            isDigit && !viet_.getRawInput().empty();

        if (!(isLetter || isVNIModifier)) {
            ic_->commitString(keyUtf8);
            continue;
        }

        std::string oldComposed = viet_.getComposed();
        auto result = viet_.processKey(ch);

        if (result == skey::ProcessResult::Committed) {
            std::string committed = viet_.getCommitted();
            viet_.clearCommitted();
            std::string newComposed = viet_.getComposed();
            std::string fullNew = committed + newComposed;
            if (!fullNew.empty()) {
                surroundingCommit(oldComposed, fullNew);
                committedLen_ = static_cast<int>(utf8::length(fullNew));
            } else {
                surroundingCommit(oldComposed, "");
                committedLen_ = 0;
            }
        } else {
            std::string newComposed = viet_.getComposed();
            if (!newComposed.empty()) {
                surroundingCommit(oldComposed, newComposed);
            } else {
                committedLen_ = 0;
            }
        }

        // If surroundingCommit triggered a new uinput replacement,
        // re-buffer remaining keys and return — they'll be replayed
        // after this new replacement completes.
        if (uinputDeleting_) {
            for (size_t j = i + 1;
                 j < keys.size() && bufferedUinputKeys_.size() < maxBufferedUinputKeys;
                 ++j) {
                bufferedUinputKeys_.push_back(keys[j]);
            }
            return;
        }
    }
}

void SKeyState::deactivate() {
    forceFlushDeferredCommit();
    if (uinputClientFd_ >= 0) {
        close(uinputClientFd_);
        uinputClientFd_ = -1;
    }
    expectedUinputBackspaces_ = 0;
    seenUinputBackspaces_ = 0;
    pendingUinputCommit_.clear();
    bufferedUinputKeys_.clear();
    bsSentAt_ = 0;
    lastBsRoundTrip_ = 0;
    uinputCommitTimer_.reset();
    uinputDeleting_ = false;
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
    bufferedUinputKeys_.clear();
    uinputCommitTimer_.reset();
    uinputDeleting_ = false;
    if (!hasDeferredCommitPending()) {
        committedLen_ = 0;
    }
    clearUI();
}

void SKeyState::keyEvent(KeyEvent &keyEvent) {
    if (keyEvent.isRelease()) {
        return;
    }

    if (handlePendingUinputBackspace(keyEvent)) {
        return;
    }

    // Buffer keys while a no-BS uinput timer commit is pending.
    // (e.g. after filterAndAccept of 'w' for 'b' → 'bư', waiting
    // for timer to commitString("ư")).
    if (uinputCommitTimer_) {
        auto sym = keyEvent.key().sym();
        std::string keyUtf8 = Key::keySymToUTF8(sym);
        if (!keyUtf8.empty() &&
            bufferedUinputKeys_.size() < maxBufferedUinputKeys) {
            SKEY_DEBUG() << "Uinput: buffer key '" << keyUtf8
                         << "' while timer pending";
            bufferedUinputKeys_.push_back(sym);
        }
        keyEvent.filterAndAccept();
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
        else if (sym == FcitxKey_4 || sym == FcitxKey_KP_4) choice = 4;

        if (choice > 0) {
            SKeyOutputMode newMode;
            switch (choice) {
            case 1: newMode = SKeyOutputMode::SurroundingText; break;
            case 2: newMode = SKeyOutputMode::Preedit; break;
            case 3: newMode = SKeyOutputMode::SurroundingTextSlow; break;
            case 4: newMode = SKeyOutputMode::Uinput; break;
            default: dismissModeMenu(); keyEvent.filterAndAccept(); return;
            }
            appModeOverride_ = newMode;
            hasAppModeOverride_ = true;
            engine_->saveAppMode(ic_->program(), newMode);
            SKEY_INFO() << "Mode switched to " << outputModeName(newMode);
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
                    std::string keyUtf8 = Key::keySymToUTF8(sym);
                    if (useUinputMode() && !keyUtf8.empty()) {
                        // Uinput mode: compute diff between oldComposed and
                        // newComposed.  Only forward the key if the result
                        // matches (matching append).  For everything else,
                        // consume the key via filterAndAccept to prevent
                        // Electron from processing it + the NEXT key (space)
                        // before the replacement completes.
                        committedLen_ = static_cast<int>(
                            utf8::length(newComposed));

                        // Check matching append: old + key == new
                        if (oldComposed + keyUtf8 == newComposed) {
                            SKEY_DEBUG() << "Uinput: forward append '"
                                         << keyUtf8 << "'";
                            return;  // forward raw key
                        }

                        // Non-matching: consume key, diff based on oldComposed
                        keyEvent.filterAndAccept();
                        size_t pfx = commonUtf8PrefixBytes(
                            oldComposed, newComposed);
                        std::string delPart = oldComposed.substr(pfx);
                        std::string addPart = newComposed.substr(pfx);
                        int deleteLen =
                            static_cast<int>(utf8::length(delPart));

                        if (deleteLen > 0) {
                            // BS via uinput, text via timer commitString
                            SKEY_DEBUG() << "Uinput: consume '" << keyUtf8
                                         << "' replace (del=" << deleteLen
                                         << " add='" << addPart << "')";
                            sendBackspaceUinput(deleteLen);
                            expectedUinputBackspaces_ = deleteLen;
                            seenUinputBackspaces_ = 0;
                            pendingUinputCommit_ = addPart;
                            uinputDeleting_ = true;
                        } else if (!addPart.empty()) {
                            if (oldComposed.empty()) {
                                // First char (e.g. 'w' → 'ư'):  synchronous
                                // commitString is safe — no pending D-Bus
                                // messages to race with.  Avoids buffering
                                // the next key and the cross-channel race
                                // that causes when its replay commitString
                                // interleaves with later uinput BS events.
                                SKEY_DEBUG() << "Uinput: consume '" << keyUtf8
                                             << "' sync commit '" << addPart
                                             << "'";
                                ic_->commitString(addPart);
                            } else {
                                // Non-first (e.g. 'b'+'w' → 'bư'): timer
                                // commit to let the prior forward reach the
                                // app before the commit string.
                                SKEY_DEBUG() << "Uinput: consume '" << keyUtf8
                                             << "' timer commit '" << addPart
                                             << "'";
                                uinputCommitTimer_ =
                                    engine_->instance()->eventLoop().addTimeEvent(
                                        CLOCK_MONOTONIC,
                                        now(CLOCK_MONOTONIC) +
                                            (lastBsRoundTrip_ > 0
                                                ? std::max(lastBsRoundTrip_ * 3, uinputCommitMinDelayUsec)
                                                : uinputCommitDefaultDelayUsec),
                                        0,
                                        [this, addPart](EventSourceTime *,
                                                        uint64_t) {
                                            uinputCommitTimer_.reset();
                                            SKEY_DEBUG() << "Uinput: timer commit '"
                                                         << addPart << "'";
                                            ic_->commitString(addPart);
                                            if (!bufferedUinputKeys_.empty()) {
                                                replayBufferedUinputKeys();
                                            }
                                            return true;
                                        });
                            }
                        }
                        return;
                    }
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
                             << (useUinputMode() ? " (uinput)"
                                                 : (needDelay ? " (slow mode, deferred)"
                                                              : " (direct)"));
                deferredBsSentAt_ = now(CLOCK_MONOTONIC);
                if (useUinputMode()) {
                    sendBackspaceUinput(deleteLen);
                    expectedUinputBackspaces_ = deleteLen;
                    seenUinputBackspaces_ = 0;
                    pendingUinputCommit_ = addedPart;
                    uinputDeleting_ = true;
                    committedLen_ = newLen;
                    return;
                }
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
    candList->setPageSize(4);
    candList->setLayoutHint(CandidateLayoutHint::Vertical);

    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "1. Surrounding Text",
        SKeyOutputMode::SurroundingText));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "2. Preedit", SKeyOutputMode::Preedit));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "3. Surrounding Text (slow)",
        SKeyOutputMode::SurroundingTextSlow));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "4. Uinput", SKeyOutputMode::Uinput));

    // Highlight the currently active mode via cursor index
    int cursorIdx = (mode == SKeyOutputMode::SurroundingText)       ? 0
                    : (mode == SKeyOutputMode::Preedit)             ? 1
                    : (mode == SKeyOutputMode::SurroundingTextSlow) ? 2
                    : (mode == SKeyOutputMode::Uinput)              ? 3
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
