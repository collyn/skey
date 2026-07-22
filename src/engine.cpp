#include "engine.h"

#include "charset.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/statusarea.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

/// Read the debug flag directly from the user config file.
/// Bypasses fcitx5's config system which doesn't reliably call setConfig() for
/// this addon. Handles both formats:
///   1. With section: [SKeyConfig] / [skey] then key=value
///   2. Without section: just key=value pairs (fcitx5 GUI output)
static bool readDebugFromFile() {
  const char *home = getenv("HOME");
  if (!home)
    return true; // default on
  std::string path = std::string(home) + "/.config/fcitx5/conf/skey.conf";
  std::ifstream f(path);
  if (!f.is_open())
    return true; // default on

  std::string line;
  bool inSection = false;
  bool fileHasSections = false;
  while (std::getline(f, line)) {
    // Trim
    size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    size_t end = line.find_last_not_of(" \t\r\n");
    line = line.substr(start, end - start + 1);

    // Skip comments
    if (line[0] == '#')
      continue;

    // Track sections
    if (line[0] == '[') {
      fileHasSections = true;
      inSection = (line == "[SKeyConfig]" || line == "[skey]");
      continue;
    }

    // If file has sections, only look inside the right one.
    // If file has NO sections, all keys are at the top level.
    if (fileHasSections && !inSection)
      continue;

    auto eq = line.find('=');
    if (eq != std::string::npos) {
      std::string key = line.substr(0, eq);
      std::string val = line.substr(eq + 1);
      if (key == "Debug") {
        return val == "True" || val == "true" || val == "1";
      }
    }
  }
  return true; // default on if key not found
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
  template <typename T> SKeyLogger &operator<<(const T &v) {
    ss_ << v;
    return *this;
  }

private:
  std::ostringstream ss_;
};

namespace fcitx {

// Timing tunables for uinput backspace → commit coordination (microseconds)
//
// Adaptive delay via EWMA (exponentially weighted moving average) of
// measured BackSpace round-trip times.  The round-trip covers:
//   uinput → kernel → X11 → Chrome → X11 → fcitx5
// Chrome's processing time varies with system load, tab count, and
// omnibox autocomplete activity.  EWMA smooths out these variations
// and automatically adjusts the commit delay — faster machines get
// lower latency, loaded machines get more safety margin.
static constexpr double kBsRtEwmaAlpha = 0.3;  // weight for new sample
static constexpr uint64_t kBsRtInitialUsec = 8000;  // seed before first sample
// Multiplier: commit delay = EWMA * multiplier.  The EWMA measures the
// BS round-trip through fcitx5, but Chrome/Electron need additional time
// to process BS internally (DOM update, layout).  3x provides 2 extra
// round-trips of headroom — enough for both Chrome and Electron apps.
static constexpr double kBsRtMultiplier = 3.0;
// Address bar: same multiplier.
static constexpr double kAddrBarBsRtMultiplier = 3.0;
// Absolute floors to prevent racing on extremely fast machines
static constexpr uint64_t kCommitDelayMinUsec = 10000;
static constexpr uint64_t kAddrBarCommitDelayMinUsec = 10000;
// dbusDeferredDefaultUsec: default delay for surrounding-text deferred commit
static constexpr uint64_t dbusDeferredDefaultUsec = 15000;
// dbusDeferredMinUsec: floor for adaptive deferred commit delay
static constexpr uint64_t dbusDeferredMinUsec = 10000;
// kUinputPassthroughMinUsec: min window to suppress engine during Ctrl+Shift+U
// typing
static constexpr uint64_t kUinputPassthroughMinUsec = 35000;
// Per-character overhead estimate for Ctrl+Shift+U hex typing (usec)
static constexpr uint64_t kUinputPerCharUsec = 10000;
// Safety timeout: if BS events don't arrive back within this window,
// force-commit and unstick the engine.  Prevents indefinite freeze
// when Chrome/Electron drops or delays BS processing.
static constexpr uint64_t kUinputSafetyTimeoutUsec = 150000; // 150ms

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
  struct passwd pwd{};
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

/// Check if a program name is a known Chromium-based browser.
static bool isChromiumBrowser(const std::string &prog) {
  // Match common Chromium browser program names
  static const char *const patterns[] = {
      "chrome",  "chromium",       "google-chrome", "brave",
      "vivaldi", "microsoft-edge", "opera",         "electron",
  };
  for (const char *p : patterns) {
    if (prog.find(p) != std::string::npos) {
      return true;
    }
  }
  return false;
}

FCITX_ADDON_FACTORY(SKeyEngineFactory);

// Candidate word for mode switch dropdown menu
class ModeCandidateWord : public CandidateWord {
public:
  ModeCandidateWord(SKeyEngine *engine, SKeyState *state,
                    const std::string &text, SKeyOutputMode mode)
      : CandidateWord(Text(text)), engine_(engine), state_(state), mode_(mode) {
  }

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

class AddressBarModeCandidateWord : public CandidateWord {
public:
  AddressBarModeCandidateWord(SKeyEngine *engine, SKeyState *state,
                              const std::string &text,
                              SKeyChromiumAddressBarMode mode)
      : CandidateWord(Text(text)), engine_(engine), state_(state), mode_(mode) {
  }

  void select(InputContext *) const override {
    engine_->setChromiumAddressBarMode(mode_);
    SKEY_INFO() << "Address bar mode switched";
    state_->dismissModeMenu();
  }

private:
  SKeyEngine *engine_;
  SKeyState *state_;
  SKeyChromiumAddressBarMode mode_;
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
    : instance_(instance), factory_([this](InputContext &ic) -> SKeyState * {
        return new SKeyState(this, &ic);
      }) {
  reloadConfig();
  instance_->inputContextManager().registerProperty("skeyState", &factory_);
  setupTrayMenu();

  // Start AT-SPI2 monitor for Chromium address bar detection.
  // Both address-bar modes (Preedit / No Vietnamese) rely on it.
  a11yMonitor_ = std::make_unique<A11yMonitor>();
  a11yMonitor_->setDebug(*config_.debug);
  a11yMonitor_->start();

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

  imMenu_.addAction(&imTelex_);
  imMenu_.addAction(&imVni_);

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

void SKeyEngine::keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) {
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

void SKeyEngine::setChromiumAddressBarMode(SKeyChromiumAddressBarMode mode) {
  config_.chromiumAddressBarMode.setValue(mode);
  safeSaveAsIni(config_, "conf/skey.conf");
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

  // Update parent label to show current selection
  if (im == SKeyInputMethod::VNI) {
    imAction_.setShortText(_("Input Method: VNI"));
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
    if (*val == "Preedit")
      return SKeyOutputMode::Preedit;
    if (*val == "SurroundingTextSlow")
      return SKeyOutputMode::SurroundingText; // migrate old config
    if (*val == "Uinput")
      return SKeyOutputMode::Uinput;
    if (*val == "SurroundingText")
      return SKeyOutputMode::SurroundingText;
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
    cfg.setValueByPath(app, ""); // clear = use default
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
  // Migrate legacy "Telex W" input method → Telex + ShortW=True.
  // The TelexW enum value no longer exists, so peek the raw ini first.
  {
    RawConfig raw;
    readAsIni(raw, "conf/skey.conf");
    auto *im = raw.valueByPath("InputMethod");
    if (im && (*im == "Telex W" || *im == "TelexW")) {
      readAsIni(config_, "conf/skey.conf");
      config_.inputMethod.setValue(SKeyInputMethod::Telex);
      config_.shortW.setValue(true);
      safeSaveAsIni(config_, "conf/skey.conf");
      SKEY_INFO() << "Migrated legacy 'Telex W' → Telex + ShortW";
    }
  }
  readAsIni(config_, "conf/skey.conf");
  g_skeyDebugEnabled = readDebugFromFile();
  if (a11yMonitor_)
    a11yMonitor_->setDebug(g_skeyDebugEnabled);
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
      FCITX_SKEY_ICON_PATH, // compile-time
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
  }
  // Charset
  switch (*cfg.charset) {
  case SKeyCharset::TCVN3:
    charset_ = skey::Charset::TCVN3;
    break;
  case SKeyCharset::VNIWindows:
    charset_ = skey::Charset::VNIWindows;
    break;
  default:
    charset_ = skey::Charset::Unicode;
    break;
  }

  viet_.setShortW(*cfg.shortW);
  viet_.setBracketUO(*cfg.bracketUO);
  viet_.setMethod(im);
  viet_.setFreeMarking(*cfg.freeMarking);
  viet_.setAutoRestore(*cfg.autoRestore);
}

void SKeyState::commitText(const std::string &utf8) {
  if (utf8.empty())
    return;
  ic_->commitString(skey::convertCharset(utf8, charset_));
}

void SKeyState::refreshAppMode() {
  std::string prog = ic_->program();
  if (prog == cachedProgram_)
    return;
  cachedProgram_ = prog;

  hasAppModeOverride_ = false;
  appExcluded_ = false;

  // IBus frontend reports empty program name (AppImages etc.).
  // Still try to load saved per-app config — the entry is keyed
  // by program name, which may be empty.
  RawConfig cfg;
  readAsIni(cfg, "conf/skey-app-modes.conf");
  auto *val = cfg.valueByPath(prog);
  if (val) {
    if (*val == "Excluded") {
      appExcluded_ = true;
    } else {
      SKeyOutputMode savedMode = engine_->config().outputMode.value();
      if (*val == "Preedit")
        savedMode = SKeyOutputMode::Preedit;
      else if (*val == "SurroundingTextSlow")
        savedMode = SKeyOutputMode::SurroundingText; // migrate
      else if (*val == "Uinput")
        savedMode = SKeyOutputMode::Uinput;
      else if (*val == "SurroundingText")
        savedMode = SKeyOutputMode::SurroundingText;
      appModeOverride_ = savedMode;
      hasAppModeOverride_ = true;
    }
  }
}

// True when the cursor is in a Chromium-family browser's address/search bar
// (as opposed to web content). Two detection paths: the native Url capability
// (Wayland) and the AT-SPI2 accessibility monitor (X11).
bool SKeyState::inChromiumAddressBar() const {
  // Method 1: Wayland — Chrome sends CapabilityFlag::Url natively
  if (ic_->capabilityFlags().test(CapabilityFlag::Url)) {
    return true;
  }
  // Method 2: X11 — use AT-SPI2 accessibility monitor
  if (engine_->a11yMonitor() && engine_->a11yMonitor()->isBrowserUIFocused() &&
      isChromiumBrowser(ic_->program())) {
    return true;
  }
  return false;
}

SKeyOutputMode SKeyState::effectiveMode() const {
  const_cast<SKeyState *>(this)->refreshAppMode();

  // Address bar output mode overrides the general and per-application modes.
  // NoVietnamese is handled as a pass-through in keyEvent().
  if (inChromiumAddressBar()) {
    switch (engine_->config().chromiumAddressBarMode.value()) {
    case SKeyChromiumAddressBarMode::Uinput:
      return SKeyOutputMode::Uinput;
    case SKeyChromiumAddressBarMode::SurroundingText:
      return SKeyOutputMode::SurroundingText;
    case SKeyChromiumAddressBarMode::Preedit:
      return SKeyOutputMode::Preedit;
    case SKeyChromiumAddressBarMode::NoVietnamese:
      break;
    }
  }

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

bool SKeyState::useHiddenComposition() const { return false; }

void SKeyState::activate() {
  // Re-sync input method from config (handles config changes at runtime)
  auto &cfg = engine_->config();
  skey::InputMethod im = skey::InputMethod::Telex;
  if (*cfg.inputMethod == SKeyInputMethod::VNI) {
    im = skey::InputMethod::VNI;
  }
  // Charset
  switch (*cfg.charset) {
  case SKeyCharset::TCVN3:
    charset_ = skey::Charset::TCVN3;
    break;
  case SKeyCharset::VNIWindows:
    charset_ = skey::Charset::VNIWindows;
    break;
  default:
    charset_ = skey::Charset::Unicode;
    break;
  }
  viet_.setShortW(*cfg.shortW);
  viet_.setBracketUO(*cfg.bracketUO);
  viet_.setMethod(im);
  viet_.setFreeMarking(*cfg.freeMarking);
  viet_.setAutoRestore(*cfg.autoRestore);

  // Reactivate after spurious cycle: cancel the genuine-loss timer.
  if (addrBarExpectCycle_) {
    SKEY_DEBUG() << "Activate: spurious cycle, cancel loss timer";
    addrBarCycleTimer_.reset();
  } else {
    viet_.reset();
    committedLen_ = 0;
    addrBarIsFirstWord_ = true;
  }
  clearLastWord();
  modeMenuActive_ = false;
  deferredCommitTimer_.reset();
  deferredCommitText_.clear();
  deferredPrefix_.clear();
  // Load per-app mode preference / exclusion.
  // IBus frontend reports empty program name — still try to load saved config.
  hasAppModeOverride_ = false;
  appExcluded_ = false;
  {
    RawConfig cfg;
    readAsIni(cfg, "conf/skey-app-modes.conf");
    auto *val = cfg.valueByPath(ic_->program());
    if (val) {
      if (*val == "Excluded") {
        appExcluded_ = true;
      } else {
        SKeyOutputMode savedMode = engine_->config().outputMode.value();
        if (*val == "Preedit")
          savedMode = SKeyOutputMode::Preedit;
        else if (*val == "SurroundingTextSlow")
          savedMode = SKeyOutputMode::SurroundingText; // migrate
        else if (*val == "Uinput")
          savedMode = SKeyOutputMode::Uinput;
        else if (*val == "SurroundingText")
          savedMode = SKeyOutputMode::SurroundingText;
        appModeOverride_ = savedMode;
        hasAppModeOverride_ = true;
      }
    }
  }

  auto caps = ic_->capabilityFlags();
  auto mode = effectiveMode();
  auto configuredMode = engine_->config().outputMode.value();
  SKEY_DEBUG() << "Activated: mode=" << outputModeName(mode)
               << " configured=" << outputModeName(configuredMode)
               << " surroundingCap="
               << caps.test(CapabilityFlag::SurroundingText)
               << " password=" << caps.test(CapabilityFlag::Password)
               << " urlCap=" << caps.test(CapabilityFlag::Url)
               << " preeditCap=" << caps.test(CapabilityFlag::Preedit)
               << " nativeSurrounding=" << useNativeSurroundingApi()
               << " frontend=" << ic_->frontendName()
               << " display=" << ic_->display() << " app=" << ic_->program()
               << " caps=0x" << std::hex
               << static_cast<uint64_t>(caps.toInteger()) << std::dec
               << " cursor=(" << ic_->cursorRect().left() << ","
               << ic_->cursorRect().top() << "," << ic_->cursorRect().width()
               << "x" << ic_->cursorRect().height() << ")";
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
  sockaddr_un addr{};
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

void SKeyState::sendBackspaceUinput(int count, const std::string &text,
                                     uint32_t flags) {
  if (count < 0) {
    return;
  }
  if (count == 0 && text.empty() && flags == 0) {
    return;
  }
  if (!connectUinputServer()) {
    SKEY_DEBUG() << "Uinput: cannot send BS, server unavailable";
    return;
  }

  // Protocol v2: int32_t count, uint32_t flags, uint32_t textLen, then text.
  // flags bit 0: send Escape before BS (dismisses Chrome autocomplete popup).
  // The server detects v1 vs v2 by message size for backward compatibility.
  int32_t count32 = count;
  uint32_t textLen = static_cast<uint32_t>(text.size());
  std::vector<char> msg(sizeof(int32_t) + sizeof(uint32_t) * 2 + textLen);
  memcpy(msg.data(), &count32, sizeof(count32));
  memcpy(msg.data() + sizeof(count32), &flags, sizeof(flags));
  memcpy(msg.data() + sizeof(count32) + sizeof(flags), &textLen, sizeof(textLen));
  if (textLen > 0) {
    memcpy(msg.data() + sizeof(count32) + sizeof(flags) + sizeof(textLen),
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
    uinputPassthroughUntil_ =
        now(CLOCK_MONOTONIC) +
        std::max(kUinputPassthroughMinUsec,
                 20000 + static_cast<uint64_t>(textLen) * kUinputPerCharUsec);
  }
}

bool SKeyState::handlePendingUinputBackspace(KeyEvent &keyEvent) {
  if (!uinputDeleting_) {
    return false;
  }

  // While waiting for BS, buffer non-BS keys (user-typed space, letters,
  // etc.) so they don't reach the app prematurely.
  // In the address bar, Chrome's spurious focus cycles can cause X11 to
  // re-deliver the trigger key — drop it to avoid double-processing.
  if (!keyEvent.key().check(FcitxKey_BackSpace) ||
      expectedUinputBackspaces_ == 0) {
    auto sym = keyEvent.key().sym();
    if (inChromiumAddressBar() && addrBarLastTriggerKey_ != 0 &&
        now(CLOCK_MONOTONIC) < addrBarTriggerDeadline_ &&
        sym == static_cast<uint32_t>(addrBarLastTriggerKey_)) {
      SKEY_DEBUG() << "Uinput: drop re-delivered trigger key 0x"
                   << std::hex << sym;
      keyEvent.filterAndAccept();
      return true;
    }
    std::string keyUtf8 = Key::keySymToUTF8(sym);
    if (!keyUtf8.empty() &&
        bufferedUinputKeys_.size() < maxBufferedUinputKeys) {
      SKEY_DEBUG() << "Uinput: buffer key '" << keyUtf8 << "' while deleting";
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
    // All BS events passed through — cancel safety timeout.
    uinputSafetyTimer_.reset();
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

    // Adaptive delay via EWMA of measured round-trip times.
    // Smooths out Chrome processing time variations (system load,
    // tab count, omnibox autocomplete activity).  Faster machines
    // naturally converge to lower delays.
    if (bsRtEwma_ == kBsRtInitialUsec || bsRtEwma_ == 0) {
      bsRtEwma_ = elapsed;  // first sample — seed directly
    } else {
      bsRtEwma_ = static_cast<uint64_t>(
          kBsRtEwmaAlpha * elapsed + (1.0 - kBsRtEwmaAlpha) * bsRtEwma_);
    }
    double multiplier = inChromiumAddressBar() ? kAddrBarBsRtMultiplier
                                                  : kBsRtMultiplier;
    uint64_t minDelay = inChromiumAddressBar() ? kAddrBarCommitDelayMinUsec
                                                : kCommitDelayMinUsec;
    uint64_t delayUsec = std::max(
        static_cast<uint64_t>(bsRtEwma_ * multiplier), minDelay);
    SKEY_DEBUG() << "Uinput: all BS passed, RT " << (elapsed / 1000)
                 << "ms (ewma " << (bsRtEwma_ / 1000)
                 << "ms), commit '" << commitText << "' in "
                 << (delayUsec / 1000) << "ms"
                 << (inChromiumAddressBar() ? " [addrbar]" : "");

    uinputCommitTimer_ = engine_->instance()->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + delayUsec, 0,
        [this, cText = commitText](EventSourceTime *, uint64_t) {
          uinputCommitTimer_.reset();
          uinputSafetyTimer_.reset();
          uinputDeleting_ = false;
          if (!cText.empty()) {
            this->commitText(cText);
          }
          if (!bufferedUinputKeys_.empty()) {
            replayBufferedUinputKeys();
          }
          return true;
        });
  }
  return true; // let BS reach the app (not consumed)
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

    flushAddrBarReplacement();
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
      for (size_t j = i + 1; j < keys.size() &&
                             bufferedUinputKeys_.size() < maxBufferedUinputKeys;
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
               << " seenBs=" << seenUinputBackspaces_ << " pendingCommit='"
               << pendingUinputCommit_ << "'";
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
  // Chromium address bar: Chrome sends spurious Reset→Deactivate→
  // Activate cycles. Skip ALL state cleanup if we're expecting a cycle.
  // If no reactivate within 500ms, this is a genuine focus loss → clear flag.
  if (addrBarExpectCycle_) {
    SKEY_DEBUG() << "Deactivate: expecting cycle, skip all cleanup";
    addrBarCycleTimer_ = engine_->instance()->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 500000, 0,
        [this](EventSourceTime *, uint64_t) {
          SKEY_DEBUG() << "Deactivate: no reactivate, genuine focus loss";
          addrBarExpectCycle_ = false;
          addrBarCycleTimer_.reset();
          // Only commit/flush if using non-uinput modes (preedit, etc.)
          // where the composition hasn't been committed yet.  In uinput
          // mode, replacements already committed via commitText() so
          // commitBuffer() here would double-commit.
          if (!useUinputMode()) {
            if (!viet_.getComposed().empty() && !useSurroundingText())
              commitBuffer();
            viet_.reset();
            committedLen_ = 0;
          }
          clearLastWord();
          clearUI();
          return true;
        });
    return;
  }

  expectedUinputBackspaces_ = 0;
  seenUinputBackspaces_ = 0;
  pendingUinputCommit_.clear();
  bufferedUinputKeys_.clear();
  bsSentAt_ = 0;
  lastBsRoundTrip_ = 0;
  bsRtEwma_ = kBsRtInitialUsec;
  uinputCommitTimer_.reset();
  uinputSafetyTimer_.reset();
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
  if (addrBarExpectCycle_) {
    SKEY_DEBUG() << "Reset: expecting cycle, skip";
    return;
  }
  if (hasDeferredCommitPending()) {
    SKEY_DEBUG() << "Reset: keeping deferred commit";
  }
  viet_.reset();
  bufferedUinputKeys_.clear();
  uinputCommitTimer_.reset();
  uinputSafetyTimer_.reset();
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
    if (keyEvent.key().check(FcitxKey_grave) && viet_.getRawInput().empty()) {
      showModeMenu();
      keyEvent.filterAndAccept();
    }
    return;
  }

  // Chromium address bar set to "No Vietnamese" — pass keys through so the
  // user types plain ASCII in the URL bar (web content is unaffected).
  if (!modeMenuActive_ &&
      engine_->config().chromiumAddressBarMode.value() ==
          SKeyChromiumAddressBarMode::NoVietnamese &&
      inChromiumAddressBar() && !keyEvent.key().check(FcitxKey_grave)) {
    return;
  }

  if (handlePendingUinputBackspace(keyEvent)) {
    return;
  }

  // Drop re-delivered trigger key after address bar replacement.
  // Chrome's spurious focus cycles cause X11 to re-send the key that
  // triggered the last replacement, even after uinput deletion completed.
  // We use a 200ms deadline so the guard doesn't stay active forever.
  if (inChromiumAddressBar() && addrBarLastTriggerKey_ != 0 &&
      now(CLOCK_MONOTONIC) < addrBarTriggerDeadline_) {
    if (keyEvent.key().sym() == static_cast<uint32_t>(addrBarLastTriggerKey_)) {
      SKEY_DEBUG() << "AddrBar: drop re-delivered trigger key 0x"
                   << std::hex << keyEvent.key().sym();
      keyEvent.filterAndAccept();
      return;
    }
  }

  // Passthrough window: after uinput types text via Ctrl+Shift+U hex,
  // the injected key events loop back through fcitx5.  Suppress engine
  // processing so those keys (Ctrl, Shift, U, hex digits, Enter) reach
  // the app unmodified instead of being interpreted as Vietnamese input.
  if (uinputPassthroughUntil_ > 0) {
    if (now(CLOCK_MONOTONIC) < uinputPassthroughUntil_) {
      return; // pass through, no engine processing
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
    if (sym == FcitxKey_1 || sym == FcitxKey_KP_1)
      choice = 1;
    else if (sym == FcitxKey_2 || sym == FcitxKey_KP_2)
      choice = 2;
    else if (sym == FcitxKey_3 || sym == FcitxKey_KP_3)
      choice = 3;
    else if (sym == FcitxKey_4 || sym == FcitxKey_KP_4)
      choice = 4;

    if (modeMenuForAddressBar_) {
      SKeyChromiumAddressBarMode newMode;
      switch (choice) {
      case 1:
        newMode = SKeyChromiumAddressBarMode::Uinput;
        break;
      case 2:
        newMode = SKeyChromiumAddressBarMode::SurroundingText;
        break;
      case 3:
        newMode = SKeyChromiumAddressBarMode::Preedit;
        break;
      case 4:
        newMode = SKeyChromiumAddressBarMode::NoVietnamese;
        break;
      default:
        newMode = SKeyChromiumAddressBarMode::Preedit;
        break;
      }
      if (choice > 0) {
        engine_->setChromiumAddressBarMode(newMode);
        SKEY_INFO() << "Address bar mode switched";
        dismissModeMenu();
        keyEvent.filterAndAccept();
        return;
      }
    } else if (choice > 0 && choice <= 3) {
      SKeyOutputMode newMode;
      switch (choice) {
      case 1:
        newMode = SKeyOutputMode::Uinput;
        break;
      case 2:
        newMode = SKeyOutputMode::SurroundingText;
        break;
      case 3:
        newMode = SKeyOutputMode::Preedit;
        break;
      default:
        break; // unreachable
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
      pendingCanMerge =
          (pendingCh >= 'a' && pendingCh <= 'z') ||
          (pendingCh >= 'A' && pendingCh <= 'Z') ||
          (engine_->config().inputMethod.value() == SKeyInputMethod::VNI &&
           pendingIsDigit && !viet_.getRawInput().empty());
    }
    if (!pendingCanMerge) {
      flushDeferredCommit();
      // If flush was deferred (BS not processed yet), we still
      // need to let the timer handle it. Pass key through.
    }
  }

  // Pass through modifier keys (except Shift and CapsLock)
  if (key.states() & ~KeyStates({KeyState::Shift, KeyState::CapsLock})) {
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
    clearLastWord(); // Arrow keys, Ctrl+X etc. invalidate retroactive editing
    return;
  }

  // Handle Backspace while composing
  if (key.check(FcitxKey_BackSpace) && !viet_.getRawInput().empty()) {
    // Chromium address bar: pass raw BS through to Chrome (X11) instead
    // of sending forwardKey via D-Bus (which triggers focus changes).
    // Just update bamboo state and let the keystroke reach the app.
    if (inChromiumAddressBar()) {
      viet_.backspace();
      committedLen_ = viet_.getRawInput().empty()
                          ? 0
                          : static_cast<int>(utf8::length(viet_.getComposed()));
      // Re-arm cycle protection and first-word flag: clearing composed text
      // puts us back in "first word" state where autocomplete may trigger.
      addrBarExpectCycle_ = true;
      if (viet_.getRawInput().empty()) {
        addrBarIsFirstWord_ = true;
      }
      SKEY_DEBUG() << "AddrBar BS: rawInput='" << viet_.getRawInput()
                   << "' composed='" << viet_.getComposed()
                   << "' len=" << committedLen_;
      return; // pass raw BS through to Chrome
    }
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

  // BS when engine is idle: use surrounding-text API for deletion on
  // Wayland (pass-through raw keys are unreliable).  Only skip for
  // Chromium address bar where raw BS pass-through works correctly.
  if (key.check(FcitxKey_BackSpace) && viet_.getRawInput().empty()) {
    if (useNativeSurroundingApi() && !inChromiumAddressBar()) {
      if (committedLen_ > 0) {
        surroundingBackspace();
        keyEvent.filterAndAccept();
        return;
      }
      // No committed text — delete one character before cursor via
      // surrounding-text API.  If we have a saved previous word, the
      // first call deletes the separator; enable one-shot reclaim so
      // the user can retype a tone key to edit the previous word.
      // Subsequent calls delete chars from the previous word itself.
      if (!lastRawInput_.empty()) {
        if (committedLen_ == 0) {
          // First call: deleting the separator
          reclaimReady_ = true;
          committedLen_ = -1; // sentinel: separator already deleted
        } else if (committedLen_ == -1) {
          // Second+ call: deleting into the previous word
          reclaimReady_ = false;
        }
        // else committedLen_ > 0: shouldn't reach here (handled above)
      }
      ic_->deleteSurroundingText(-1, 1);
      if (ic_->surroundingText().isValid()) {
        ic_->surroundingText().deleteText(-1, 1);
      }
      SKEY_DEBUG() << "SurrBS: delete 1 via surrounding text";
      keyEvent.filterAndAccept();
      return;
    }
    // Non-native-surrounding path: pass through.
    if (!lastRawInput_.empty()) {
      reclaimReady_ = true;
    }
    // Re-arm cycle protection for address bar (see composing BS handler).
    if (inChromiumAddressBar()) {
      addrBarExpectCycle_ = true;
    }
    return; // pass through
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
      // After space, the next word is NOT the first word — autocomplete
      // won't trigger on multi-word text.
      addrBarIsFirstWord_ = false;
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
    // Telex "][→ươ" option: '[' / ']' become composition keys.
    bool isBracketKey =
        (ch == '[' || ch == ']') && engine_->config().bracketUO.value() &&
        engine_->config().inputMethod.value() == SKeyInputMethod::Telex;

    if (isLetter || isVNIModifier || isBracketKey) {
      // Retroactive tone editing (Unikey-style): if the user has
      // backspaced into the previous word and types a tone modifier
      // key (s/f/r/x/j for Telex, 1-5/0 for VNI), reclaim the last
      // word so the tone can be changed.
      bool didReclaim = false;
      if (viet_.getRawInput().empty() && !lastRawInput_.empty() &&
          reclaimReady_ && useSurroundingText()) {
        auto im = engine_->config().inputMethod.value();
        bool isToneKey = false;
        if (im == SKeyInputMethod::Telex) {
          isToneKey = (ch == 's' || ch == 'f' || ch == 'r' || ch == 'x' ||
                       ch == 'j' || ch == 'z');
        } else if (im == SKeyInputMethod::VNI) {
          isToneKey = (ch >= '0' && ch <= '5');
        }
        if (isToneKey) {
          reclaimLastWord();
          didReclaim = true;
        }
        reclaimReady_ = false;
      }

      // Flush any pending address bar replacement before processing
      // a new key, so the screen state matches viet_'s expectation.
      flushAddrBarReplacement();

      std::string oldComposed = viet_.getComposed();

      auto result = viet_.processKey(ch);

      if (didReclaim) {
        std::string newComposed = viet_.getComposed();
        std::string keyUtf8(1, ch);
        bool justAppend = (newComposed == oldComposed + keyUtf8);
        bool autoRestored = (newComposed == viet_.getRawInput());

        if (justAppend || autoRestored ||
            result == skey::ProcessResult::Committed) {
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
          // Set trigger-key guard for Chromium (see same logic below).
          if (isChromiumBrowser(ic_->program()) && !oldComposed.empty()) {
            addrBarLastTriggerKey_ = static_cast<int>(sym);
            addrBarTriggerDeadline_ = now(CLOCK_MONOTONIC) + 200000;
          }
          std::string fullNew = committed + newComposed;
          if (!fullNew.empty()) {
            surroundingCommit(oldComposed, fullNew);
            committedLen_ = static_cast<int>(utf8::length(fullNew));
          } else {
            // Both committed and newComposed are empty — just
            // delete old text from screen
            surroundingCommit(oldComposed, "");
            committedLen_ = 0;
          }
        } else {
          if (!committed.empty()) {
            commitText(committed);
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
      SKEY_DEBUG() << "Key '" << ch << "': old='" << oldComposed << "' new='"
                   << newComposed << "' len=" << committedLen_;

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
            committedLen_ = static_cast<int>(utf8::length(newComposed));

            // Check matching append: old + key == new
            if (oldComposed + keyUtf8 == newComposed) {
              // Forward raw X11 key — instant, no D-Bus latency.
              // Set cycle protection: subsequent replacement may trigger
              // spurious focus changes in Chromium address bar.
              if (inChromiumAddressBar()) {
                addrBarExpectCycle_ = true;
              }
              SKEY_DEBUG() << "Uinput: forward append '" << keyUtf8 << "'";
              return; // forward raw key
            }

            // Non-matching: consume key, send BS via uinput,
            // replacement text via commitString with adaptive
            // delay to let the app process uinput BS first.
            keyEvent.filterAndAccept();
            size_t pfx = commonUtf8PrefixBytes(oldComposed, newComposed);
            std::string delPart = oldComposed.substr(pfx);
            std::string addPart = newComposed.substr(pfx);
            int deleteLen = static_cast<int>(utf8::length(delPart));

            if (deleteLen > 0) {
              SKEY_DEBUG() << "Uinput: consume '" << keyUtf8
                           << "' replace (del=" << deleteLen << " add='"
                           << addPart << "')";
              // Chromium address bar on X11: use forwardKey (D-Bus)
              // for BackSpace instead of uinput so both BS and
              // commitString travel the same channel — D-Bus ordering
              // guarantees commitString arrives after BS is processed.
              if (inChromiumAddressBar()) {
                committedLen_ = static_cast<int>(utf8::length(newComposed));
                scheduleAddrBarReplacement(deleteLen, addPart,
                                           static_cast<int>(utf8::length(oldComposed)),
                                           static_cast<int>(sym),
                                           newComposed);
                return;
              }
              sendBackspaceUinput(deleteLen);
              expectedUinputBackspaces_ = deleteLen;
              seenUinputBackspaces_ = 0;
              pendingUinputCommit_ = addPart;
              uinputDeleting_ = true;
              // Safety: force-commit if BS events are lost
              uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
                  CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + kUinputSafetyTimeoutUsec, 0,
                  [this](EventSourceTime *, uint64_t) {
                    SKEY_DEBUG() << "Uinput: safety timeout, force commit";
                    uinputSafetyTimer_.reset();
                    uinputCommitTimer_.reset();
                    std::string text = std::move(pendingUinputCommit_);
                    pendingUinputCommit_.clear();
                    expectedUinputBackspaces_ = 0;
                    seenUinputBackspaces_ = 0;
                    uinputDeleting_ = false;
                    if (!text.empty()) this->commitText(text);
                    if (!bufferedUinputKeys_.empty()) replayBufferedUinputKeys();
                    return true;
                  });
            } else if (!addPart.empty()) {
              SKEY_DEBUG() << "Uinput: consume '" << keyUtf8 << "' commit '"
                           << addPart << "'";
              if (inChromiumAddressBar()) addrBarExpectCycle_ = true;
              commitText(addPart);
            }
            committedLen_ = static_cast<int>(utf8::length(newComposed));
            return;
          }
          // SurroundingText path in Chromium: set trigger-key guard so
          // X11 re-delivery after forwardKey-induced focus cycles is dropped.
          // Only for replacements (oldComposed non-empty), not first char.
          if (isChromiumBrowser(ic_->program()) &&
              !oldComposed.empty() && oldComposed != newComposed) {
            addrBarLastTriggerKey_ = static_cast<int>(sym);
            addrBarTriggerDeadline_ = now(CLOCK_MONOTONIC) + 200000;
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
    commitText(text);
  }
  viet_.reset();
  // After committing a word, the next one is not the first.
  addrBarIsFirstWord_ = false;
}

void SKeyState::scheduleAddrBarReplacement(int bs, const std::string &text,
                                             int oldComposedLen,
                                             int triggerKeySym,
                                             const std::string &fullComposed) {
  // Use uinput BS + adaptive EWMA timer delay before D-Bus commitString.
  // uinput BS goes through kernel → Chrome processes it as a real keystroke
  // (omnibox update, autocomplete dismissal).  The EWMA-based delay adapts
  // to Chrome's actual processing speed — faster machines get lower latency.
  addrBarExpectCycle_ = true;
  addrBarLastTriggerKey_ = triggerKeySym;
  addrBarTriggerDeadline_ = now(CLOCK_MONOTONIC) + 200000;
  if (bs > 0) {
    int totalBs = bs;
    std::string commitText = text;
    if (addrBarIsFirstWord_ && oldComposedLen > 0 && !fullComposed.empty()) {
      // First word in address bar: Chrome's inline autocomplete may consume
      // the first BS (dismissing a suggestion instead of deleting a char).
      // Delete the ENTIRE old word + 1 extra BS, and commit the FULL new
      // composed text.  This way:
      //   - autocomplete eats 1 BS → remaining BS delete old word → correct
      //   - no autocomplete → old word deleted + 1 harmless BS on empty → correct
      // Safe because there is no prior text to damage.
      totalBs = oldComposedLen + 1;
      commitText = fullComposed;
      SKEY_DEBUG() << "AddrBar: first word, fullReplace BS=" << totalBs
                   << " commit='" << commitText << "'";
    }
    addrBarIsFirstWord_ = false;
    sendBackspaceUinput(totalBs);
    expectedUinputBackspaces_ = totalBs;
    seenUinputBackspaces_ = 0;
    pendingUinputCommit_ = commitText;
    uinputDeleting_ = true;
    // Safety: force-commit if BS events are lost
    uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + kUinputSafetyTimeoutUsec, 0,
        [this](EventSourceTime *, uint64_t) {
          SKEY_DEBUG() << "Uinput: safety timeout, force commit";
          uinputSafetyTimer_.reset();
          uinputCommitTimer_.reset();
          std::string text = std::move(pendingUinputCommit_);
          pendingUinputCommit_.clear();
          expectedUinputBackspaces_ = 0;
          seenUinputBackspaces_ = 0;
          uinputDeleting_ = false;
          if (!text.empty()) this->commitText(text);
          if (!bufferedUinputKeys_.empty()) replayBufferedUinputKeys();
          return true;
        });
  } else if (!text.empty()) {
    this->commitText(text);
  }
}

void SKeyState::flushAddrBarReplacement() {
  // If a uinput replacement is in flight (BS sent, waiting to commit),
  // flush it synchronously so the next key operates on the correct state.
  if (uinputDeleting_ && !pendingUinputCommit_.empty() &&
      expectedUinputBackspaces_ == 0) {
    uinputCommitTimer_.reset();
    uinputDeleting_ = false;
    SKEY_DEBUG() << "AddrBar: flush pending uinput commit '"
                 << pendingUinputCommit_ << "'";
    commitText(pendingUinputCommit_);
    pendingUinputCommit_.clear();
    if (!bufferedUinputKeys_.empty()) {
      replayBufferedUinputKeys();
    }
  }
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
  uint64_t delayUsec =
      (bsRtEwma_ > 0 && bsRtEwma_ != kBsRtInitialUsec)
          ? std::max(bsRtEwma_ * 2 + 8000, dbusDeferredMinUsec)
          : dbusDeferredDefaultUsec;

  SKEY_DEBUG() << "Surr deferred: schedule '" << text << "' in "
               << (delayUsec / 1000) << "ms";
  deferredCommitTimer_ = engine_->instance()->eventLoop().addTimeEvent(
      CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + delayUsec, 0,
      [this](EventSourceTime *, uint64_t) {
        SKEY_DEBUG() << "Surr deferred: timer commit '" << deferredCommitText_
                     << "'";
        std::string toCommit = deferredCommitText_ + pendingFlushSuffix_;
        deferredCommitText_.clear();
        deferredPrefix_.clear();
        deferredBsSentAt_ = 0;
        pendingFlushSuffix_.clear();
        deferredCommitTimer_.reset();
        commitText(toCommit);
        return true;
      });
}

void SKeyState::flushDeferredCommit() {
  if (!hasDeferredCommitPending()) {
    deferredCommitTimer_.reset();
    return;
  }

  // Enforce adaptive minimum delay between BackSpace and commit.
  uint64_t minGapUsec =
      (bsRtEwma_ > 0 && bsRtEwma_ != kBsRtInitialUsec)
          ? std::max(bsRtEwma_ * 2 + 8000, dbusDeferredMinUsec)
          : dbusDeferredDefaultUsec;
  if (deferredBsSentAt_ > 0) {
    uint64_t nowUs = now(CLOCK_MONOTONIC);
    uint64_t elapsed = nowUs - deferredBsSentAt_;
    if (elapsed < minGapUsec) {
      // Not safe to commit yet — reschedule for the remaining time.
      uint64_t remaining = minGapUsec - elapsed;
      SKEY_DEBUG() << "Surr deferred: flush delayed " << (remaining / 1000)
                   << "ms (BS not processed)";
      deferredCommitTimer_.reset();
      deferredCommitTimer_ = engine_->instance()->eventLoop().addTimeEvent(
          CLOCK_MONOTONIC, nowUs + remaining, 0,
          [this](EventSourceTime *, uint64_t) {
            SKEY_DEBUG() << "Surr deferred: delayed flush commit '"
                         << deferredCommitText_ << "'";
            std::string toCommit = deferredCommitText_ + pendingFlushSuffix_;
            deferredCommitText_.clear();
            deferredPrefix_.clear();
            deferredBsSentAt_ = 0;
            pendingFlushSuffix_.clear();
            deferredCommitTimer_.reset();
            commitText(toCommit);
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
  commitText(toCommit);
}

void SKeyState::forceFlushDeferredCommit() {
  if (!hasDeferredCommitPending()) {
    deferredCommitTimer_.reset();
    return;
  }
  // Force-commit immediately — don't wait for BS timing.
  // Used at word boundaries (space/enter/punctuation) to prevent
  // stale deferred commits from corrupting new word composition.
  SKEY_DEBUG() << "Surr deferred: force flush commit '" << deferredCommitText_
               << "'";
  std::string toCommit = deferredCommitText_ + pendingFlushSuffix_;
  deferredCommitText_.clear();
  deferredPrefix_.clear();
  deferredBsSentAt_ = 0;
  pendingFlushSuffix_.clear();
  deferredCommitTimer_.reset();
  commitText(toCommit);
}

void SKeyState::surroundingCommit(const std::string &oldComposed,
                                  const std::string &newComposed) {
  if (newComposed.empty())
    return;

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
    commitText(newComposed);
    committedLen_ = newLen;
  } else if (isSimpleAppend) {
    std::string appended = newComposed.substr(oldComposed.size());
    SKEY_DEBUG() << "Surr: append '" << appended << "'";
    commitText(appended);
    committedLen_ = newLen;
  } else {
    size_t commonPrefix = commonUtf8PrefixBytes(oldComposed, newComposed);
    std::string deletedPart = oldComposed.substr(commonPrefix);
    std::string addedPart = newComposed.substr(commonPrefix);
    std::string stablePrefix = newComposed.substr(0, commonPrefix);
    int deleteLen = static_cast<int>(utf8::length(deletedPart));

    SKEY_DEBUG() << "Surr: replace '" << oldComposed << "' -> '" << newComposed
                 << "' (delete suffix x" << deleteLen << ")";
    if (deleteLen > 0) {
      // Helper: delete via BackSpace forwarding, then commit.
      // D-Bus guarantees message ordering within a connection, so
      // commitString always arrives after the forwarded BackSpace
      // keys — no timer needed.
      auto deleteViaBackspace = [&]() {
        SKEY_DEBUG() << "Surr: BS x" << deleteLen
                     << (useUinputMode() ? " (uinput)" : " (forward)");
        // Chromium address bar: use uinput BS + buffering for both
        // Uinput and SurroundingText modes, avoiding D-Bus forwardKey
        // focus-change issues.
        if (inChromiumAddressBar()) {
          committedLen_ = newLen;
          scheduleAddrBarReplacement(deleteLen, addedPart,
                                     static_cast<int>(utf8::length(oldComposed)),
                                     0, newComposed);
          return;
        }
        if (useUinputMode()) {
          sendBackspaceUinput(deleteLen);
          expectedUinputBackspaces_ = deleteLen;
          seenUinputBackspaces_ = 0;
          pendingUinputCommit_ = addedPart;
          uinputDeleting_ = true;
          // Safety: force-commit if BS events are lost
          uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
              CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + kUinputSafetyTimeoutUsec, 0,
              [this](EventSourceTime *, uint64_t) {
                SKEY_DEBUG() << "Uinput: safety timeout, force commit";
                uinputSafetyTimer_.reset();
                uinputCommitTimer_.reset();
                std::string text = std::move(pendingUinputCommit_);
                pendingUinputCommit_.clear();
                expectedUinputBackspaces_ = 0;
                seenUinputBackspaces_ = 0;
                uinputDeleting_ = false;
                if (!text.empty()) this->commitText(text);
                if (!bufferedUinputKeys_.empty()) replayBufferedUinputKeys();
                return true;
              });
          committedLen_ = newLen;
          return;
        }
        // SurroundingText forwardKey path: D-Bus forwardKey may trigger
        // Chrome focus cycles (omnibox autocomplete).  Protect engine state
        // even when inChromiumAddressBar() wasn't detected (AT-SPI2 race).
        if (isChromiumBrowser(ic_->program())) {
          addrBarExpectCycle_ = true;
        }
        for (int i = 0; i < deleteLen; ++i) {
          ic_->forwardKey(Key(FcitxKey_BackSpace));
        }
        committedLen_ = newLen;
        if (!addedPart.empty()) {
          commitText(addedPart);
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
            commitText(addedPart);
          }
        }
      } else {
        SKEY_DEBUG() << "Surr: client has no surrounding text capability";
        deleteViaBackspace();
      }
    } else {
      // deleteLen == 0: no deletion needed, only add new suffix if any
      if (!addedPart.empty()) {
        commitText(addedPart);
      }
      committedLen_ = newLen;
    }
  }
}

void SKeyState::surroundingBackspace() {
  if (committedLen_ <= 0)
    return;

  SKEY_DEBUG() << "SurrBS: delete surrounding 1, len=" << committedLen_
               << " -> " << (committedLen_ - 1);

  if (useNativeSurroundingApi()) {
    ic_->deleteSurroundingText(-1, 1);
    if (ic_->surroundingText().isValid()) {
      ic_->surroundingText().deleteText(-1, 1);
    }
  } else {
    SKEY_DEBUG()
        << "SurrBS: client has no surrounding text capability, fallback BS";
    ic_->forwardKey(Key(FcitxKey_BackSpace));
  }
  committedLen_--;

  // Reset engine so next keystroke starts fresh composition.
  // The old committed chars (before cursor) stay in the app untouched.
  viet_.reset();
}

void SKeyState::saveLastWord() {
  if (viet_.getRawInput().empty())
    return;
  lastRawInput_ = viet_.getRawInput();
  lastComposed_ = viet_.getComposed();
  lastCommittedLen_ = committedLen_;
  SKEY_DEBUG() << "SaveLastWord: raw='" << lastRawInput_ << "' composed='"
               << lastComposed_ << "' len=" << lastCommittedLen_;
}

void SKeyState::clearLastWord() {
  lastRawInput_.clear();
  lastComposed_.clear();
  lastCommittedLen_ = 0;
  reclaimReady_ = false;
}

void SKeyState::reclaimLastWord() {
  SKEY_DEBUG() << "ReclaimLastWord: raw='" << lastRawInput_ << "' composed='"
               << lastComposed_ << "' len=" << lastCommittedLen_;

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
    if (lead < 0x80) {
      cp = lead;
      seqLen = 1;
    } else if (lead < 0xC0) {
      i++;
      continue; /* continuation byte, skip */
    } else if (lead < 0xE0) {
      cp = lead & 0x1F;
      seqLen = 2;
    } else if (lead < 0xF0) {
      cp = lead & 0x0F;
      seqLen = 3;
    } else {
      cp = lead & 0x07;
      seqLen = 4;
    }
    for (int j = 1; j < seqLen && i + j < text.size(); j++) {
      cp = (cp << 6) | (static_cast<uint8_t>(text[i + j]) & 0x3F);
    }
    i += seqLen;
    // Unicode keysym range: 0x01000000 + codepoint
    ic_->forwardKey(Key(static_cast<KeySym>(0x01000000 | cp)));
  }
  SKEY_DEBUG() << "Surr: forwarded '" << text << "' as " << utf8::length(text)
               << " key events";
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
  modeMenuForAddressBar_ = inChromiumAddressBar();
  auto mode = effectiveMode();

  // Build candidate list for dropdown menu
  auto candList = std::make_unique<CommonCandidateList>();
  candList->setPageSize(4);
  candList->setLayoutHint(CandidateLayoutHint::Vertical);

  int cursorIdx = 0;
  if (modeMenuForAddressBar_) {
    auto addressBarMode = engine_->config().chromiumAddressBarMode.value();
    candList->append(std::make_unique<AddressBarModeCandidateWord>(
        engine_, this, "1. Uinput", SKeyChromiumAddressBarMode::Uinput));
    candList->append(std::make_unique<AddressBarModeCandidateWord>(
        engine_, this, "2. Surrounding Text",
        SKeyChromiumAddressBarMode::SurroundingText));
    candList->append(std::make_unique<AddressBarModeCandidateWord>(
        engine_, this, "3. Preedit", SKeyChromiumAddressBarMode::Preedit));
    candList->append(std::make_unique<AddressBarModeCandidateWord>(
        engine_, this, "4. Không gõ tiếng Việt",
        SKeyChromiumAddressBarMode::NoVietnamese));
    cursorIdx = static_cast<int>(addressBarMode);
  } else {
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "1. Uinput", SKeyOutputMode::Uinput));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "2. Surrounding Text", SKeyOutputMode::SurroundingText));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "3. Preedit", SKeyOutputMode::Preedit));

    std::string excludeLabel =
        appExcluded_ ? "4. ✓ Loại trừ ứng dụng" : "4. Loại trừ ứng dụng";
    candList->append(
        std::make_unique<ExcludeCandidateWord>(engine_, this, excludeLabel));

    cursorIdx = appExcluded_                                ? 3
                : (mode == SKeyOutputMode::Uinput)          ? 0
                : (mode == SKeyOutputMode::SurroundingText) ? 1
                : (mode == SKeyOutputMode::Preedit)         ? 2
                                                            : 0;
  }
  candList->setGlobalCursorIndex(cursorIdx);

  ic_->inputPanel().setCandidateList(std::move(candList));
  ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
  SKEY_DEBUG() << "Menu: mode switch dropdown shown (cursor=" << cursorIdx
               << ")";
}

void SKeyState::dismissModeMenu() {
  modeMenuActive_ = false;
  modeMenuForAddressBar_ = false;
  ic_->inputPanel().setCandidateList(nullptr);
  clearUI();
}

} // namespace fcitx
