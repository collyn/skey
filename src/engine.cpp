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
#include <unistd.h>
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

/// Read the debug flag directly from the user config file.
/// Bypasses fcitx5's config system which doesn't reliably call setConfig() for this addon.
/// Handles both formats:
///   1. With section: [SKeyConfig] / [skey] then key=value
///   2. Without section: just key=value pairs (fcitx5 GUI output)
static bool readDebugFromFile() {
    const char *home = getenv("HOME");
    if (!home) return true;  // default on
    std::string path = std::string(home) + "/.config/fcitx5/conf/skey.conf";
    std::ifstream f(path);
    if (!f.is_open()) return true;  // default on

    std::string line;
    bool inSection = false;
    bool fileHasSections = false;
    while (std::getline(f, line)) {
        // Trim
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        // Skip comments
        if (line[0] == '#') continue;

        // Track sections
        if (line[0] == '[') {
            fileHasSections = true;
            inSection = (line == "[SKeyConfig]" || line == "[skey]");
            continue;
        }

        // If file has sections, only look inside the right one.
        // If file has NO sections, all keys are at the top level.
        if (fileHasSections && !inSection) continue;

        auto eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if (key == "Debug") {
                return val == "True" || val == "true" || val == "1";
            }
        }
    }
    return true;  // default on if key not found
}

static bool g_skeyDebugEnabled = true;

class SKeyLogger {
public:
    ~SKeyLogger() {
        if (!g_skeyDebugEnabled) {
            return;
        }
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
    case SKeyOutputMode::Uinput:
        return "Uinput";
    }
    return "SurroundingText";
}

static constexpr size_t maxBufferedUinputKeys = 32;


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

class ExcludeCandidateWord : public CandidateWord {
public:
    ExcludeCandidateWord(SKeyEngine *engine, SKeyState *state,
                         const std::string &text)
        : CandidateWord(Text(text)), engine_(engine), state_(state) {}

    void select(InputContext *) const override {
        bool newExcluded = !state_->appExcluded_;
        state_->appExcluded_ = newExcluded;
        engine_->saveAppExcluded(state_->ic_->program(), newExcluded);
        SKEY_INFO() << "App '" << state_->ic_->program()
                    << (newExcluded ? "' excluded" : "' included");
        state_->dismissModeMenu();
    }

private:
    SKeyEngine *engine_;
    SKeyState *state_;
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
    omUinput_.setShortText(_("Uinput"));
    omUinput_.setCheckable(true);
    omUinput_.registerAction("skey-om-uinput", &uiManager);
    omSurrounding_.setShortText(_("Surrounding Text"));
    omSurrounding_.setCheckable(true);
    omSurrounding_.registerAction("skey-om-surrounding", &uiManager);
    omPreedit_.setShortText(_("Preedit"));
    omPreedit_.setCheckable(true);
    omPreedit_.registerAction("skey-om-preedit", &uiManager);

    omMenu_.addAction(&omUinput_);
    omMenu_.addAction(&omSurrounding_);
    omMenu_.addAction(&omPreedit_);

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
    omUinput_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        FCITX_UNUSED(ic);
        setOutputMode(SKeyOutputMode::Uinput);
    });

    // ── Settings action ──
    settingsAction_.setShortText(_("Settings..."));
    settingsAction_.registerAction("skey-settings", &uiManager);
    settingsAction_.connect<SimpleAction::Activated>([this](InputContext *ic) {
        FCITX_UNUSED(ic);
        pid_t pid = fork();
        if (pid == 0) {
            execlp("fcitx5-skey-settings", "fcitx5-skey-settings", nullptr);
            _exit(1);
        }
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

    // Re-read config to pick up runtime changes (e.g. Debug toggle)
    reloadConfig();

    // Add tray menu actions (InputMethod group is cleared before activate)
    ic->statusArea().addAction(StatusGroup::InputMethod, &imAction_);
    ic->statusArea().addAction(StatusGroup::InputMethod, &omAction_);
    ic->statusArea().addAction(StatusGroup::InputMethod, &settingsAction_);
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
    // Preserve Debug from file — fcitx5's config system may not include it
    config_.debug.setValue(readDebugFromFile());
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
    omUinput_.setChecked(om == SKeyOutputMode::Uinput);

    if (om == SKeyOutputMode::Preedit) {
        omAction_.setShortText(_("Output Mode: Preedit"));
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
        if (*val == "SurroundingTextSlow") return SKeyOutputMode::SurroundingText;  // migrate old config
        if (*val == "Uinput") return SKeyOutputMode::Uinput;
        if (*val == "SurroundingText") return SKeyOutputMode::SurroundingText;
    }
    // Not found — return the configured default
    return config_.outputMode.value();
}

void SKeyEngine::saveAppExcluded(const std::string &app, bool excluded) {
    RawConfig cfg;
    readAsIni(cfg, "conf/skey-app-modes.conf");
    if (excluded) {
        cfg.setValueByPath(app, "Excluded");
    } else {
        cfg.setValueByPath(app, "");  // clear = use default
    }
    safeSaveAsIni(cfg, "conf/skey-app-modes.conf");
    SKEY_INFO() << "App '" << app << "' " << (excluded ? "excluded" : "included");
}

bool SKeyEngine::isAppExcluded(const std::string &app) const {
    RawConfig cfg;
    readAsIni(cfg, "conf/skey-app-modes.conf");
    auto *val = cfg.valueByPath(app);
    return val && *val == "Excluded";
}

void SKeyEngine::reloadConfig() {
    readAsIni(config_, "conf/skey.conf");
    g_skeyDebugEnabled = readDebugFromFile();
    std::string modeStr = outputModeName(config_.outputMode.value());
    SKEY_INFO() << "Config: outputMode=" << modeStr
                << " debug(from file)=" << g_skeyDebugEnabled;
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

std::string SKeyEngine::subModeIconImpl(const InputMethodEntry &entry,
                                        InputContext &ic) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(ic);
    // Return absolute path to bypass XDG icon theme lookup, which fails on
    // many non-Breeze KDE icon themes despite the icon being installed in
    // hicolor and breeze fallback directories.
    static const std::vector<std::string> candidates = {
        FCITX_SKEY_ICON_PATH,                                        // compile-time
        "/usr/share/icons/hicolor/scalable/status/fcitx-skey.svg",
        "/usr/share/icons/hicolor/scalable/apps/fcitx-skey.svg",
        "/usr/share/icons/hicolor/48x48/apps/fcitx-skey.png",
    };
    static std::string cachedPath;
    if (cachedPath.empty()) {
        for (const auto &p : candidates) {
            if (access(p.c_str(), R_OK) == 0) {
                cachedPath = p;
                break;
            }
        }
    }
    return cachedPath;
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

void SKeyState::refreshAppMode() {
    std::string prog = ic_->program();
    if (prog == cachedProgram_)
        return;
    cachedProgram_ = prog;

    hasAppModeOverride_ = false;
    appExcluded_ = false;
    if (prog.empty())
        return;

    RawConfig cfg;
    readAsIni(cfg, "conf/skey-app-modes.conf");
    auto *val = cfg.valueByPath(prog);
    if (val) {
        if (*val == "Excluded") {
            appExcluded_ = true;
        } else {
            SKeyOutputMode savedMode = engine_->config().outputMode.value();
            if (*val == "Preedit") savedMode = SKeyOutputMode::Preedit;
            else if (*val == "SurroundingTextSlow") savedMode = SKeyOutputMode::SurroundingText;  // migrate
            else if (*val == "Uinput") savedMode = SKeyOutputMode::Uinput;
            else if (*val == "SurroundingText") savedMode = SKeyOutputMode::SurroundingText;
            appModeOverride_ = savedMode;
            hasAppModeOverride_ = true;
        }
    }
}

SKeyOutputMode SKeyState::effectiveMode() const {
    const_cast<SKeyState *>(this)->refreshAppMode();
    if (hasAppModeOverride_)
        return appModeOverride_;
    return engine_->config().outputMode.value();
}

bool SKeyState::useSurroundingText() const {
    auto mode = effectiveMode();
    return mode == SKeyOutputMode::SurroundingText ||
           mode == SKeyOutputMode::Uinput;
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
    clearLastWord();
    modeMenuActive_ = false;
    deferredCommitTimer_.reset();
    deferredCommitText_.clear();
    deferredPrefix_.clear();
    // Load per-app mode preference / exclusion
    hasAppModeOverride_ = false;
    appExcluded_ = false;
    if (!ic_->program().empty()) {
        RawConfig cfg;
        readAsIni(cfg, "conf/skey-app-modes.conf");
        auto *val = cfg.valueByPath(ic_->program());
        if (val) {
            if (*val == "Excluded") {
                appExcluded_ = true;
            } else {
                SKeyOutputMode savedMode = engine_->config().outputMode.value();
                if (*val == "Preedit") savedMode = SKeyOutputMode::Preedit;
                else if (*val == "SurroundingTextSlow") savedMode = SKeyOutputMode::SurroundingText;  // migrate
                else if (*val == "Uinput") savedMode = SKeyOutputMode::Uinput;
                else if (*val == "SurroundingText") savedMode = SKeyOutputMode::SurroundingText;
                appModeOverride_ = savedMode;
                hasAppModeOverride_ = true;
            }
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

void SKeyState::sendBackspaceUinput(int count, const std::string &text) {
    if (count < 0) {
        return;
    }
    if (count == 0 && text.empty()) {
        return;
    }
    if (!connectUinputServer()) {
        SKEY_DEBUG() << "Uinput: cannot send BS, server unavailable";
        return;
    }

    // Protocol: int32_t count, uint32_t textLen, then text bytes.
    // The server sends `count` BS events, then types `text` via Ctrl+Shift+U.
    int32_t count32 = count;
    uint32_t textLen = static_cast<uint32_t>(text.size());
    std::vector<char> msg(sizeof(int32_t) + sizeof(uint32_t) + textLen);
    memcpy(msg.data(), &count32, sizeof(count32));
    memcpy(msg.data() + sizeof(count32), &textLen, sizeof(textLen));
    if (textLen > 0) {
        memcpy(msg.data() + sizeof(count32) + sizeof(textLen),
               text.data(), textLen);
    }

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
    SKEY_DEBUG() << "Uinput: sent BS=" << count
                 << (textLen > 0 ? " +text='" + text + "'" : "");
    // When text is included, the server types it via Ctrl+Shift+U hex.
    // Those keystrokes loop back through fcitx5; enable passthrough so
    // they reach the app unmodified.  Window is sized per character
    // (roughly 15ms per char for Ctrl+Shift+U typing + latency).
    if (textLen > 0) {
        uinputPassthroughUntil_ = now(CLOCK_MONOTONIC) +
            std::max(kUinputPassthroughMinUsec,
                     20000 + static_cast<uint64_t>(textLen) * 15000);
    }
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
        // All BS events passed through the IM module — the app has
        // received them.  But D-Bus commitString may still race with
        // kernel event processing in the app's event loop.  Small
        // delay (5ms) lets the app finish processing the last BS.
        expectedUinputBackspaces_ = 0;
        seenUinputBackspaces_ = 0;

        std::string commitText = pendingUinputCommit_;
        pendingUinputCommit_.clear();

        uint64_t elapsed = now(CLOCK_MONOTONIC) - bsSentAt_;
        lastBsRoundTrip_ = elapsed;

        // BS (uinput) and commit (D-Bus) go through different channels.
        // Wait at least until the BS round-trip has elapsed + 8ms pad,
        // to give the app time to process the uinput BS before commitString.
        static constexpr uint64_t kMinDelayUsec = 8000;  // 8ms floor
        uint64_t delayUsec = std::max(elapsed + kMinDelayUsec, kMinDelayUsec);
        SKEY_DEBUG() << "Uinput: all BS passed, round-trip " << (elapsed / 1000)
                     << "ms, commit '" << commitText << "' in "
                     << (delayUsec / 1000) << "ms";

        uinputCommitTimer_ = engine_->instance()->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC,
            now(CLOCK_MONOTONIC) + delayUsec,
            0,
            [this, commitText](EventSourceTime *, uint64_t) {
                uinputCommitTimer_.reset();
                uinputDeleting_ = false;
                if (!commitText.empty()) {
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
            // Word boundary (space, etc.) — finalize current composition
            if (!viet_.getRawInput().empty()) {
                viet_.reset();
                committedLen_ = 0;
            }
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
            // Non-letter printable (punctuation, etc.) — finalize current composition
            if (!viet_.getRawInput().empty()) {
                viet_.reset();
                committedLen_ = 0;
            }
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
    SKEY_DEBUG() << "Deactivate: deleting=" << uinputDeleting_
                 << " pendingBs=" << expectedUinputBackspaces_
                 << " seenBs=" << seenUinputBackspaces_
                 << " pendingCommit='" << pendingUinputCommit_ << "'";
    forceFlushDeferredCommit();

    // Chrome address bar (and other apps) can trigger focus changes during
    // BS round-trips.  If all BS events have passed but the 5ms commit
    // timer hasn't fired yet, flush synchronously so the replacement text
    // isn't lost.  Otherwise the app ends up with only the deleted chars.
    if (uinputDeleting_ && !pendingUinputCommit_.empty() &&
        expectedUinputBackspaces_ == 0) {
        uinputCommitTimer_.reset();
        uinputDeleting_ = false;
        SKEY_DEBUG() << "Deactivate: flush pending uinput commit '"
                     << pendingUinputCommit_ << "'";
        ic_->commitString(pendingUinputCommit_);
        pendingUinputCommit_.clear();
        if (!bufferedUinputKeys_.empty()) {
            replayBufferedUinputKeys();
        }
    }

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
    clearLastWord();
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
    clearLastWord();
    clearUI();
}

void SKeyState::keyEvent(KeyEvent &keyEvent) {
    if (keyEvent.isRelease()) {
        return;
    }

    // Refresh per-app mode in case IC is shared across apps
    refreshAppMode();

    // App excluded — pass all keys through, except backtick for menu
    if (appExcluded_ && !modeMenuActive_) {
        if (keyEvent.key().check(FcitxKey_grave) &&
            viet_.getRawInput().empty()) {
            showModeMenu();
            keyEvent.filterAndAccept();
        }
        return;
    }

    if (handlePendingUinputBackspace(keyEvent)) {
        return;
    }

    // Passthrough window: after uinput types text via Ctrl+Shift+U hex,
    // the injected key events loop back through fcitx5.  Suppress engine
    // processing so those keys (Ctrl, Shift, U, hex digits, Enter) reach
    // the app unmodified instead of being interpreted as Vietnamese input.
    if (uinputPassthroughUntil_ > 0) {
        if (now(CLOCK_MONOTONIC) < uinputPassthroughUntil_) {
            return;  // pass through, no engine processing
        }
        uinputPassthroughUntil_ = 0;
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

        if (choice > 0 && choice <= 3) {
            SKeyOutputMode newMode;
            switch (choice) {
            case 1: newMode = SKeyOutputMode::Uinput; break;
            case 2: newMode = SKeyOutputMode::SurroundingText; break;
            case 3: newMode = SKeyOutputMode::Preedit; break;
            default: break;  // unreachable
            }
            appExcluded_ = false;
            engine_->saveAppExcluded(ic_->program(), false);
            appModeOverride_ = newMode;
            hasAppModeOverride_ = true;
            engine_->saveAppMode(ic_->program(), newMode);
            SKEY_INFO() << "Mode switched to " << outputModeName(newMode);
            dismissModeMenu();
            keyEvent.filterAndAccept();
            return;
        } else if (choice == 4) {
            bool newExcluded = !appExcluded_;
            appExcluded_ = newExcluded;
            engine_->saveAppExcluded(ic_->program(), newExcluded);
            SKEY_INFO() << "App '" << ic_->program()
                        << (newExcluded ? "' excluded" : "' included");
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
            saveLastWord();
            if (useSurroundingText()) {
                forceFlushDeferredCommit();
            } else {
                commitBuffer();
                clearUI();
            }
            viet_.reset();
            committedLen_ = 0;
        }
        clearLastWord();  // Arrow keys, Ctrl+X etc. invalidate retroactive editing
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

    // BS when engine is idle: pass through to app (deletes separator like
    // space), but KEEP lastRawInput_ and enable retroactive reclaim.
    if (key.check(FcitxKey_BackSpace) && viet_.getRawInput().empty()) {
        if (!lastRawInput_.empty()) {
            reclaimReady_ = true;
        }
        return;  // pass through
    }

    // Handle Escape
    if (key.check(FcitxKey_Escape) && !viet_.getRawInput().empty()) {
        viet_.reset();
        committedLen_ = 0;
        clearLastWord();
        if (!useSurroundingText()) {
            clearUI();
        }
        keyEvent.filterAndAccept();
        return;
    }

    // Handle Enter
    if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
        if (!viet_.getRawInput().empty()) {
            saveLastWord();
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
            saveLastWord();
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
            saveLastWord();
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
            // Retroactive tone editing (Unikey-style): if the user has
            // backspaced into the previous word and types a tone modifier
            // key (s/f/r/x/j for Telex, 1-5/0 for VNI), reclaim the last
            // word so the tone can be changed.
            bool didReclaim = false;
            if (viet_.getRawInput().empty() && !lastRawInput_.empty()
                && reclaimReady_ && useSurroundingText()) {
                auto im = engine_->config().inputMethod.value();
                bool isToneKey = false;
                if (im == SKeyInputMethod::Telex || im == SKeyInputMethod::TelexW) {
                    isToneKey = (ch == 's' || ch == 'f' || ch == 'r'
                                 || ch == 'x' || ch == 'j' || ch == 'z');
                } else if (im == SKeyInputMethod::VNI) {
                    isToneKey = (ch >= '0' && ch <= '5');
                }
                if (isToneKey) {
                    reclaimLastWord();
                    didReclaim = true;
                }
                reclaimReady_ = false;
            }

            std::string oldComposed = viet_.getComposed();

            auto result = viet_.processKey(ch);

            if (didReclaim) {
                std::string newComposed = viet_.getComposed();
                std::string keyUtf8(1, ch);
                bool justAppend = (newComposed == oldComposed + keyUtf8);
                bool autoRestored = (newComposed == viet_.getRawInput());

                if (justAppend || autoRestored
                    || result == skey::ProcessResult::Committed) {
                    // Tone key didn't produce a useful change — undo reclaim.
                    SKEY_DEBUG() << "Reclaim: no transform for '" << ch
                                 << "', undo reclaim";
                    viet_.reset();
                    viet_.clearCommitted();
                    committedLen_ = 0;

                    // Re-process the key as the start of a new word
                    oldComposed = "";
                    result = viet_.processKey(ch);
                }
            }

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

                        // Non-matching: consume key, send BS via uinput,
                        // replacement text via commitString with adaptive
                        // delay to let the app process uinput BS first.
                        keyEvent.filterAndAccept();
                        size_t pfx = commonUtf8PrefixBytes(
                            oldComposed, newComposed);
                        std::string delPart = oldComposed.substr(pfx);
                        std::string addPart = newComposed.substr(pfx);
                        int deleteLen =
                            static_cast<int>(utf8::length(delPart));

                        if (deleteLen > 0) {
                            SKEY_DEBUG() << "Uinput: consume '" << keyUtf8
                                         << "' replace (del=" << deleteLen
                                         << " add='" << addPart << "')";
                            sendBackspaceUinput(deleteLen);
                            expectedUinputBackspaces_ = deleteLen;
                            seenUinputBackspaces_ = 0;
                            pendingUinputCommit_ = addPart;
                            uinputDeleting_ = true;
                        } else if (!addPart.empty()) {
                            SKEY_DEBUG() << "Uinput: consume '" << keyUtf8
                                         << "' commit '" << addPart << "'";
                            ic_->commitString(addPart);
                        }
                        committedLen_ = static_cast<int>(
                            utf8::length(newComposed));
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
            saveLastWord();
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
        saveLastWord();
        if (useSurroundingText()) {
            forceFlushDeferredCommit();
        } else {
            commitBuffer();
            clearUI();
        }
        viet_.reset();
        committedLen_ = 0;
    } else {
        // Non-composing key (Home, End, etc.) invalidates retroactive editing
        clearLastWord();
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
    // events before we commit new text via commitString.
    //
    // forwardKey(BS) and commitString both go through the same D-Bus
    // connection (fcitx5 → app), so FIFO ordering is guaranteed.
    // This delay covers the app's internal processing time (event
    // dispatch → DOM update) between receiving BS and being ready
    // for the commit.
    //
    // Adaptive: reuse uinput round-trip measurement as a proxy for
    // the app's event loop speed.  D-Bus forwardKey has ~2× overhead
    // vs kernel uinput, so we scale accordingly.
    //
    // During fast typing this delay is invisible: each keystroke resets
    // the timer via the deferred update path in surroundingCommit(), so
    // the commit only happens when the user pauses (natural word boundary).
    static constexpr uint64_t dbusDeferredDefaultUsec = 20000;  // 20ms
    static constexpr uint64_t dbusDeferredMinUsec     = 10000;  // 10ms floor
    uint64_t delayUsec = (lastBsRoundTrip_ > 0)
        ? std::max(lastBsRoundTrip_ * 2 + 8000, dbusDeferredMinUsec)
        : dbusDeferredDefaultUsec;

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

    // Enforce adaptive minimum delay between BackSpace and commit.
    static constexpr uint64_t dbusDeferredDefaultUsec = 20000;
    static constexpr uint64_t dbusDeferredMinUsec     = 10000;
    uint64_t minGapUsec = (lastBsRoundTrip_ > 0)
        ? std::max(lastBsRoundTrip_ * 2 + 8000, dbusDeferredMinUsec)
        : dbusDeferredDefaultUsec;
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
            // D-Bus guarantees message ordering within a connection, so
            // commitString always arrives after the forwarded BackSpace
            // keys — no timer needed.
            auto deleteViaBackspace = [&]() {
                SKEY_DEBUG() << "Surr: BS x" << deleteLen
                             << (useUinputMode() ? " (uinput)" : " (forward)");
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
                    ic_->commitString(addedPart);
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

void SKeyState::saveLastWord() {
    if (viet_.getRawInput().empty()) return;
    lastRawInput_ = viet_.getRawInput();
    lastComposed_ = viet_.getComposed();
    lastCommittedLen_ = committedLen_;
    SKEY_DEBUG() << "SaveLastWord: raw='" << lastRawInput_
                 << "' composed='" << lastComposed_
                 << "' len=" << lastCommittedLen_;
}

void SKeyState::clearLastWord() {
    lastRawInput_.clear();
    lastComposed_.clear();
    lastCommittedLen_ = 0;
    reclaimReady_ = false;
}

void SKeyState::reclaimLastWord() {
    SKEY_DEBUG() << "ReclaimLastWord: raw='" << lastRawInput_
                 << "' composed='" << lastComposed_
                 << "' len=" << lastCommittedLen_;

    // Restore the VietnameseEngine state from the saved raw input.
    // We do NOT modify the screen — the composed text already matches
    // what's displayed.  The next key the user types will go through
    // the normal processKey() + surroundingCommit() flow to update the
    // screen (e.g., typing 's' after "vãi" changes it to "vái").
    //
    // This avoids complex screen replacement in uinput mode where:
    //   - Ctrl+Shift+U text typing is app-dependent (fails in many apps)
    //   - Async BS counting can stall for seconds in some apps (Telegram)
    std::string savedRaw = lastRawInput_;
    int savedLen = lastCommittedLen_;

    // Clear last word state (now being reclaimed)
    clearLastWord();

    // Feed saved raw input back into the engine to reconstruct composition
    for (char ch : savedRaw) {
        viet_.processKey(ch);
    }
    viet_.clearCommitted();

    committedLen_ = savedLen;

    SKEY_DEBUG() << "ReclaimLastWord: restored raw='" << viet_.getRawInput()
                 << "' composed='" << viet_.getComposed()
                 << "' committedLen=" << committedLen_;
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
        engine_, this, "1. Uinput", SKeyOutputMode::Uinput));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "2. Surrounding Text",
        SKeyOutputMode::SurroundingText));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "3. Preedit", SKeyOutputMode::Preedit));

    // Exclude/Include toggle
    std::string excludeLabel = appExcluded_
        ? "4. ✓ Loại trừ ứng dụng"
        : "4. Loại trừ ứng dụng";
    candList->append(std::make_unique<ExcludeCandidateWord>(
        engine_, this, excludeLabel));

    // Highlight the currently active mode via cursor index
    int cursorIdx = appExcluded_                                ? 3
                    : (mode == SKeyOutputMode::Uinput)          ? 0
                    : (mode == SKeyOutputMode::SurroundingText) ? 1
                    : (mode == SKeyOutputMode::Preedit)         ? 2
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
