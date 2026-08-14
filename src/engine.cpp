#include "engine.h"

#include "charset.h"
#include "icon_resolver.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx-utils/standardpaths.h>

// StandardPath is deprecated in favour of StandardPaths in newer fcitx5,
// but we use it for compatibility with older versions (CI / LTS distros).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
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
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
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
// measured BackSpace round-trip times.  Chrome's processing time varies
// with system load, tab count, and omnibox autocomplete activity.
// EWMA smooths out these variations and automatically adjusts the commit
// delay — faster machines get lower latency, loaded machines get more
// safety margin.
//
// Separate values for X11 vs Wayland: Wayland has lower latency because
// there is no X server in the path (uinput → kernel → compositor → app),
// so the commit delay can be shorter than on X11 where the round-trip
// includes X server serialization (uinput → kernel → X11 → app → X11 →
// fcitx5).

struct UinputTiming {
  double bsRtEwmaAlpha;         // weight for new EWMA sample
  uint64_t bsRtInitialUsec;     // seed before first sample
  double bsRtMultiplier;        // commit delay = EWMA * multiplier
  double addrBarBsRtMultiplier; // same, for address bar
  uint64_t commitDelayMinUsec;  // absolute floor
  uint64_t addrBarCommitDelayMinUsec;
  uint64_t commitDelayMaxUsec; // absolute ceiling (prevents lag on slow apps)
  uint64_t addrBarCommitDelayMaxUsec;
  double chromiumDelayFactor;   // extra multiplier for Chromium/Electron apps
  uint64_t safetyTimeoutUsec;   // force-commit if BS events don't arrive
  uint64_t safetyRetryUsec;     // extended window when BS are just slow
  uint64_t passthroughBaseUsec; // base window for Ctrl+Shift+U passthrough
  uint64_t passthroughMinUsec;  // min Ctrl+Shift+U passthrough window
  uint64_t perCharUsec;         // per-char overhead for Ctrl+Shift+U typing
};

// X11: sync BS approach — send N+1 BS, the extra one anchors ordering.
// By the time the (N+1)-th BS arrives back at fcitx5, X11 has serialized
// all N real BS to the app.  A short adaptive sleep replaces the old
// async EWMA×3 commit timer — lower latency, no race condition.
static constexpr UinputTiming kUinputTimingX11 = {
    0.3,    // bsRtEwmaAlpha
    5000,   // bsRtInitialUsec
    1.5,    // bsRtMultiplier (sync BS guarantees ordering, lower multiplier)
    3.0,    // addrBarBsRtMultiplier (unchanged)
    3000,   // commitDelayMinUsec — 3ms floor for native
    10000,  // addrBarCommitDelayMinUsec — 10ms (unchanged)
    30000,  // commitDelayMaxUsec — 30ms cap for native
    50000,  // addrBarCommitDelayMaxUsec — 50ms (unchanged)
    2.0,    // chromiumDelayFactor — 2× → 6ms–60ms for Electron/Chromium
    150000, // safetyTimeoutUsec (150ms)
    600000, // safetyRetryUsec (600ms) — one extension for slow loopbacks
    20000,  // passthroughBaseUsec
    35000,  // passthroughMinUsec
    10000,  // perCharUsec
};

// Wayland: clamp delay to [2ms, 12ms] for native apps.
// Electron/Chromium apps get a 1.5× boost via chromiumDelayFactor.
static constexpr UinputTiming kUinputTimingWayland = {
    0.3,   // bsRtEwmaAlpha — moderate adaptation
    3000,  // bsRtInitialUsec
    0.5,   // bsRtMultiplier
    0.5,   // addrBarBsRtMultiplier
    2000,  // commitDelayMinUsec — 2ms floor (prevents char loss on Qt/GTK)
    2000,  // addrBarCommitDelayMinUsec
    12000, // commitDelayMaxUsec — 12ms cap
    12000, // addrBarCommitDelayMaxUsec
    1.5,   // chromiumDelayFactor — Electron multi-process needs 1.5×
    80000, // safetyTimeoutUsec (80ms)
    600000, // safetyRetryUsec (600ms) — one extension for slow loopbacks
    500,   // passthroughBaseUsec
    500,   // passthroughMinUsec
    1500,  // perCharUsec
};

// Surr deferred commit timing (same mechanism, independent of uinput path)
static constexpr uint64_t dbusDeferredDefaultUsec = 15000;
static constexpr uint64_t dbusDeferredMinUsec = 10000;

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

/// True when the surrounding-text cache verifiably ends (right before the
/// cursor) with `expected` — i.e. the cache still reflects the text we
/// believe is on screen.  After BackSpace storms or key re-delivery,
/// Chromium (Wayland) may not push fresh surrounding text; a stale cache
/// makes deleteSurroundingText remove the wrong characters (or nothing),
/// and the follow-up commit then duplicates text (retyping "thật" after
/// deleting it ends up as "thâtật").
/// SurroundingText::cursor() is a character offset into text().
static bool surroundingCacheEndsWith(const fcitx::SurroundingText &st,
                                     const std::string &expected) {
  if (!st.isValid() || expected.empty()) {
    return false;
  }
  const std::string &txt = st.text();
  if (txt.size() < expected.size()) {
    return false;
  }
  // Convert the character cursor position to a byte offset.
  size_t curBytes = 0;
  for (unsigned int charsLeft = st.cursor();
       charsLeft > 0 && curBytes < txt.size(); --charsLeft) {
    ++curBytes;
    while (curBytes < txt.size() &&
           (static_cast<unsigned char>(txt[curBytes]) & 0xC0) == 0x80) {
      ++curBytes;
    }
  }
  if (curBytes < expected.size()) {
    return false;
  }
  return txt.compare(curBytes - expected.size(), expected.size(), expected) ==
         0;
}

static std::string outputModeName(SKeyOutputMode mode) {
  switch (mode) {
  case SKeyOutputMode::SurroundingText:
    return "Surrounding Text";
  case SKeyOutputMode::Preedit:
    return "Preedit";
  case SKeyOutputMode::Uinput:
    return "Uinput";
  case SKeyOutputMode::Auto:
    return "Auto";
  }
  return "Surrounding Text";
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
// Matches actual Chromium-family browser programs.
// Used ONLY for address-bar detection — electron/tabby must NOT match here
// or non-browser Electron apps are misidentified as Chrome address bar.
static bool isChromiumBrowser(const std::string &prog) {
  static const char *const patterns[] = {
      "chrome",  "chromium",       "google-chrome", "brave",
      "vivaldi", "microsoft-edge", "opera",
  };
  for (const char *p : patterns) {
    if (prog.find(p) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Matches Chromium-family browsers AND Electron-based apps.
// Electron apps share Chromium's multi-process architecture and need the
// same BackSpace→commit timing adjustments.  Use this for everything
// EXCEPT inChromiumAddressBar(), which must use the narrower
// isChromiumBrowser() to avoid false positives.
static bool isChromiumBasedApp(const std::string &prog) {
  if (prog.empty())
    return false;
  if (isChromiumBrowser(prog))
    return true;
  if (prog.find("electron") != std::string::npos)
    return true;

  // Scan /proc for processes matching prog, then check whether the
  // binary or any ancestor links to electron/chrome/chromium.  This
  // catches renamed Electron shells (antigravity-ide, Tabby, etc.)
  // without hardcoding app names.
  DIR *dir = opendir("/proc");
  if (!dir)
    return false;

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_type != DT_DIR)
      continue;
    if (!isdigit(entry->d_name[0]))
      continue;

    std::string pidStr = entry->d_name;

    // Read comm — truncated to 15 chars by the kernel, so compare
    // against the first 15 chars of prog as well.
    std::string commPath = "/proc/" + pidStr + "/comm";
    std::ifstream commFile(commPath);
    if (!commFile.is_open())
      continue;
    std::string comm;
    std::getline(commFile, comm);
    if (!comm.empty() && comm.back() == '\n')
      comm.pop_back();

    if (comm != prog && prog.compare(0, 15, comm) != 0 &&
        comm.compare(0, 15, prog) != 0) {
      // Also try cmdline (first arg = binary path, may contain prog)
      std::string cmdPath = "/proc/" + pidStr + "/cmdline";
      std::ifstream cmdFile(cmdPath);
      if (cmdFile.is_open()) {
        std::string cmdline;
        std::getline(cmdFile, cmdline, '\0');
        // Extract basename
        size_t slash = cmdline.rfind('/');
        std::string basename =
            (slash != std::string::npos) ? cmdline.substr(slash + 1) : cmdline;
        if (basename.empty() ||
            (basename != prog && basename.find(prog) == std::string::npos &&
             prog.find(basename) == std::string::npos))
          continue;
      } else {
        continue;
      }
    }

    // Found matching process — check ancestry for Chromium markers
    int ppid = 0;
    for (int depth = 0; depth < 10; depth++) {
      std::string checkPid = (depth == 0) ? pidStr : std::to_string(ppid);

      // Check exe symlink
      char exeBuf[PATH_MAX];
      std::string exePath = "/proc/" + checkPid + "/exe";
      ssize_t len = readlink(exePath.c_str(), exeBuf, sizeof(exeBuf) - 1);
      if (len > 0) {
        exeBuf[len] = '\0';
        std::string exe(exeBuf);
        std::string lower = exe;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("electron") != std::string::npos ||
            lower.find("chrome") != std::string::npos ||
            lower.find("chromium") != std::string::npos) {
          closedir(dir);
          return true;
        }

        // chrome-sandbox in the same directory = Electron app
        size_t lastSlash = exe.rfind('/');
        if (lastSlash != std::string::npos) {
          std::string exeDir = exe.substr(0, lastSlash);
          if (access((exeDir + "/chrome-sandbox").c_str(), F_OK) == 0) {
            closedir(dir);
            return true;
          }
          if (access((exeDir + "/chrome_crashpad_handler").c_str(), F_OK) ==
              0) {
            closedir(dir);
            return true;
          }
        }
      }

      // Walk up to parent
      if (depth == 0) {
        std::string statPath = "/proc/" + pidStr + "/stat";
        std::ifstream statFile(statPath);
        if (!statFile.is_open())
          break;
        std::string statLine;
        std::getline(statFile, statLine);
        size_t rparen = statLine.rfind(')');
        if (rparen == std::string::npos)
          break;
        std::istringstream iss(statLine.substr(rparen + 2));
        std::string state;
        iss >> state >> ppid;
      } else {
        std::string pstatPath = "/proc/" + std::to_string(ppid) + "/stat";
        std::ifstream pstatFile(pstatPath);
        if (!pstatFile.is_open())
          break;
        std::string pstatLine;
        std::getline(pstatFile, pstatLine);
        size_t rparen = pstatLine.rfind(')');
        if (rparen == std::string::npos)
          break;
        std::istringstream piss(pstatLine.substr(rparen + 2));
        std::string state;
        int newPpid;
        piss >> state >> newPpid;
        if (newPpid <= 1 || newPpid == ppid)
          break;
        ppid = newPpid;
      }
    }
  }
  closedir(dir);
  return false;
}

static bool isTerminalApp(const std::string &prog) {
  // Terminals have their own internal buffer — SurroundingText API
  // doesn't sync correctly, so Uinput raw key pass-through works better.
  // Electron terminals (Tabby, Hyper) advertise the SurroundingText
  // capability but apply delete_surrounding_text unreliably (deletes
  // dropped or reordered against commitString), corrupting every toned
  // word — route them to Uinput like native terminals.
  static const char *const patterns[] = {
      "konsole",        "org.kde.konsole",
      "alacritty",      "kitty",
      "gnome-terminal", "xfce4-terminal",
      "sterm",          "st-",
      "terminator",     "terminology",
      "wezterm",        "foot",
      "urxvt",          "rxvt",
      "xterm",
      "tabby",          "hyper",
  };
  for (const char *p : patterns) {
    if (prog.find(p) != std::string::npos) {
      return true;
    }
  }
  return false;
}

/// Detect lock screen / login screen programs by name.
/// These should NEVER process Vietnamese transforms — the user is
/// typing a password.  CapabilityFlag::PasswordOrSensitive (Wayland)
/// and AT-SPI2 (X11) catch most cases, but some lock screens (like
/// KDE's kscreenlocker_greet on X11) don't expose either signal.
static bool programIsLockScreen(const std::string &prog) {
  static const char *const patterns[] = {
      "kscreenlocker",     // KDE lock screen (kscreenlocker_greet)
      "i3lock",            // i3 lock screen
      "swaylock",          // Sway lock screen
      "gtklock",           // GTK-based lock screen
      "hyprlock",          // Hyprland lock screen
      "sddm",              // SDDM login manager
      "gdm",               // GDM login manager
      "lightdm",           // LightDM login manager
      "lxdm",              // LXDM login manager
      "polkit",            // polkit auth dialogs
  };
  for (const char *p : patterns) {
    if (prog.find(p) != std::string::npos) {
      return true;
    }
  }
  // Exact match for system auth programs (substring would be too broad)
  if (prog == "login" || prog == "su" || prog == "sudo" ||
      prog == "pkexec") {
    return true;
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
    state_->modeCacheValid_ = false;
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
    state_->modeCacheValid_ = false;
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
  omAuto_.setShortText(_("Auto"));
  omAuto_.setCheckable(true);
  omAuto_.registerAction("skey-om-auto", &uiManager);
  omUinput_.setShortText(_("Uinput"));
  omUinput_.setCheckable(true);
  omUinput_.registerAction("skey-om-uinput", &uiManager);
  omSurrounding_.setShortText(_("Surrounding Text"));
  omSurrounding_.setCheckable(true);
  omSurrounding_.registerAction("skey-om-surrounding", &uiManager);
  omPreedit_.setShortText(_("Preedit"));
  omPreedit_.setCheckable(true);
  omPreedit_.registerAction("skey-om-preedit", &uiManager);

  omMenu_.addAction(&omAuto_);
  omMenu_.addAction(&omUinput_);
  omMenu_.addAction(&omSurrounding_);
  omMenu_.addAction(&omPreedit_);

  omAction_.setShortText(_("Output Mode"));
  omAction_.setMenu(&omMenu_);
  omAction_.registerAction("skey-output-mode", &uiManager);

  omAuto_.connect<SimpleAction::Activated>([this](InputContext *ic) {
    FCITX_UNUSED(ic);
    setOutputMode(SKeyOutputMode::Auto);
  });
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
  auto *ic = event.inputContext();

  // Re-read config to pick up runtime changes (e.g. Debug toggle)
  reloadConfig();

  // Sync the IM entry icon (context menu / IM list) with the current theme.
  // subModeIconImpl handles the tray indicator, but the addon/IM list icon
  // comes from InputMethodEntry::icon(), which defaults to the static
  // Icon= in the .conf file.  Update it whenever the theme changes.
  std::string currentTheme = config_.iconTheme.value();
  if (iconCacheTheme_ != currentTheme) {
    // Force subModeIconImpl to resolve and cache the new path
    iconCacheTheme_.clear();
    iconCachePath_.clear();
    std::string resolved = subModeIconImpl(entry, *ic);
    const_cast<InputMethodEntry &>(entry).setIcon(resolved);
  }

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

void SKeyEngine::save() {
  safeSaveAsIni(macroTableConfig_, "conf/skey-macro.conf");
}

const Configuration *SKeyEngine::getConfig() const { return &config_; }

const Configuration *SKeyEngine::getSubConfig(const std::string &path) const {
  if (path == "skey-macro") {
    return &macroTableConfig_;
  }
  return nullptr;
}

void SKeyEngine::setSubConfig(const std::string &path,
                              const RawConfig &config) {
  if (path == "skey-macro") {
    macroTableConfig_.load(config, true);
    safeSaveAsIni(macroTableConfig_, "conf/skey-macro.conf");
    rebuildMacroLookup();
  }
}

void SKeyEngine::setConfig(const RawConfig &config) {
  config_.load(config, true);
  // If fcitx5 didn't include Debug in the incoming config (legacy),
  // preserve the existing file value. Otherwise trust the incoming value.
  if (!config.valueByPath("Debug")) {
    config_.debug.setValue(readDebugFromFile());
  }
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
  bool ok = safeSaveAsIni(config_, "conf/skey.conf");
  SKEY_INFO() << "AddrBar mode saved: " << static_cast<int>(mode)
              << " ok=" << ok;
}

std::string SKeyEngine::lookupMacro(const std::string &key) const {
  if (key.empty() || macroTable_.empty())
    return "";
  auto it = macroTable_.find(key);
  if (it != macroTable_.end())
    return it->second;
  // Case-insensitive lookup
  std::string lower = key;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  it = macroTable_.find(lower);
  return (it != macroTable_.end()) ? it->second : "";
}

void SKeyEngine::rebuildMacroLookup() {
  macroTable_.clear();
  for (const auto &entry : *macroTableConfig_.entries) {
    if (!entry.key->empty() && !entry.value->empty()) {
      std::string lower = *entry.key;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      macroTable_[lower] = *entry.value;
    }
  }
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
  omAuto_.setChecked(om == SKeyOutputMode::Auto);
  omSurrounding_.setChecked(om == SKeyOutputMode::SurroundingText);
  omPreedit_.setChecked(om == SKeyOutputMode::Preedit);
  omUinput_.setChecked(om == SKeyOutputMode::Uinput);

  if (om == SKeyOutputMode::Auto) {
    omAction_.setShortText(_("Output Mode: Auto"));
  } else if (om == SKeyOutputMode::Preedit) {
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
  bool ok = safeSaveAsIni(cfg, "conf/skey-app-modes.conf");
  SKEY_INFO() << "Saved app mode: " << app << " -> " << val
              << " ok=" << ok;
}

SKeyOutputMode SKeyEngine::loadAppMode(const std::string &app) const {
  RawConfig cfg;
  readAsIni(cfg, "conf/skey-app-modes.conf");
  auto *val = cfg.valueByPath(app);
  if (val) {
    if (*val == "Preedit")
      return SKeyOutputMode::Preedit;
    if (*val == "SurroundingTextSlow" || *val == "SurroundingText" ||
        *val == "Surrounding Text")
      return SKeyOutputMode::SurroundingText;
    if (*val == "Uinput")
      return SKeyOutputMode::Uinput;
    if (*val == "Auto")
      return SKeyOutputMode::Auto;
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

  // Parse mode-menu key from config string (default: "grave" = backtick `)
  modeMenuKey_ = Key(config_.modeMenuKey.value());

  // Load macro table from fcitx5 config system.
  // Create empty file on first run so configtool discovers the sub-config.
  {
    const char *home = getenv("HOME");
    std::string macroPath = (home ? std::string(home) : "/tmp") +
                            "/.config/fcitx5/conf/skey-macro.conf";
    if (access(macroPath.c_str(), F_OK) != 0) {
      safeSaveAsIni(macroTableConfig_, "conf/skey-macro.conf");
    }
  }
  readAsIni(macroTableConfig_, "conf/skey-macro.conf");
  rebuildMacroLookup();
  if (!macroTable_.empty()) {
    SKEY_INFO() << "Loaded " << macroTable_.size() << " macro(s)";
  }

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
  // Cache keyed on the current IconTheme — re-resolves automatically when
  // the config value changes (no manual invalidation needed).
  std::string currentTheme = config_.iconTheme.value();
  if (iconCacheTheme_ == currentTheme && !iconCachePath_.empty())
    return iconCachePath_;

  iconCacheTheme_ = currentTheme;

  // ── Cinnamon: return icon NAME (not absolute path) ──────────────────
  // Cinnamon's tray uses XApp Status Applet (SNI).  The IconName property
  // is sent over D-Bus and resolved via Gtk.IconTheme — which only
  // understands theme icon names, not filesystem paths.  This matches
  // how fcitx5-bamboo works: it never overrides subModeIconImpl, so
  // the icon name from the .conf file is used directly.
  //
  // On KDE and GNOME, absolute paths work correctly — their compositors
  // or SNI hosts handle filesystem paths in IconName.  Keep the v0.5.5
  // absolute-path behavior there.
  static const bool kIsCinnamon = [] {
    const char *de = std::getenv("XDG_CURRENT_DESKTOP");
    if (!de) de = std::getenv("DESKTOP_SESSION");
    return de && (std::string(de) == "cinnamon" ||
                  std::string(de) == "X-Cinnamon");
  }();

  if (kIsCinnamon && skey::isPresetTheme(currentTheme)) {
    iconCachePath_ = skey::presetIconBaseName(currentTheme);
    return iconCachePath_;
  }

  // ── Default: resolve to absolute path (KDE, GNOME, etc.) ───────────
  // Return absolute path to bypass XDG icon theme lookup, which fails on
  // many non-Breeze KDE icon themes despite the icon being installed in
  // hicolor and breeze fallback directories.
  skey::IconSearchPaths paths;
  // fcitx5's PkgData = "$XDG_DATA_HOME/fcitx5" (~/.local/share/fcitx5)
  paths.userDataDir =
      fcitx::StandardPaths::global()
          .userDirectory(fcitx::StandardPathsType::PkgData)
          .string();
  // SVG-first: DE compositors render SVGs natively for tray icons
  paths.systemDirs = {
      "/usr/share/icons/hicolor/scalable/apps",
      "/usr/share/icons/hicolor/scalable/status",
      "/usr/share/icons/hicolor/48x48/apps",
      "/usr/share/pixmaps",
  };
  paths.fallback = FCITX_SKEY_ICON_PATH; // compile-time default

  iconCachePath_ = skey::resolveIconPath(currentTheme, paths);
  return iconCachePath_;
}

#pragma GCC diagnostic pop

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
  // Charset — SKeyCharset and Charset enums share the same order
  charset_ = static_cast<skey::Charset>(*cfg.charset);

  viet_.setShortW(*cfg.shortW);
  viet_.setBracketUO(*cfg.bracketUO);
  viet_.setMethod(im);
  viet_.setFreeMarking(*cfg.freeMarking);
  viet_.setAutoRestore(*cfg.autoRestore);
  viet_.setDict(*cfg.dict);
  loadUserDict();
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
    std::string modeStr = *val;
    if (modeStr.size() >= 2 && modeStr.front() == '"' &&
        modeStr.back() == '"')
      modeStr = modeStr.substr(1, modeStr.size() - 2);

    if (modeStr == "Excluded") {
      appExcluded_ = true;
    } else {
      SKeyOutputMode savedMode = engine_->config().outputMode.value();
      if (modeStr == "Preedit")
        savedMode = SKeyOutputMode::Preedit;
      else if (modeStr == "SurroundingTextSlow" ||
               modeStr == "SurroundingText" ||
               modeStr == "Surrounding Text")
        savedMode = SKeyOutputMode::SurroundingText;
      else if (modeStr == "Uinput")
        savedMode = SKeyOutputMode::Uinput;
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
  // Method 2: X11 — use AT-SPI2 accessibility monitor.
  // Only on X11 where Chrome does NOT send CapabilityFlag::Url natively.
  // On Wayland, Chrome accurately reports Url for the address bar (urlCap=1)
  // and omits it for the find bar (urlCap=0).  Using the AT-SPI2 fallback on
  // Wayland would misclassify the Ctrl+F find bar as an address bar, causing
  // Escape-key autocomplete dismissal to close the find bar.
  if (!isWayland() && engine_->a11yMonitor() &&
      engine_->a11yMonitor()->isBrowserUIFocused() &&
      isChromiumBrowser(ic_->program())) {
    return true;
  }
  return false;
}

// Detect browser autofill/autocomplete suggestions via surrounding text
// selection.  When Chrome shows autocomplete in the address bar, it selects
// the suggested text from cursor to end-of-line.  This is more robust than
// sending Escape — it works in any browser UI (address bar, find bar, search
// box) and avoids accidentally closing UI elements like the Ctrl+F find bar.
//
bool SKeyState::isAutofillCertain() const {
  const auto &surrounding = ic_->surroundingText();
  if (!surrounding.isValid())
    return false;

  unsigned int cursor = surrounding.cursor();
  unsigned int anchor = surrounding.anchor();
  if (cursor == anchor)
    return false; // no selection = no autocomplete

  unsigned int selStart = std::min(anchor, cursor);
  unsigned int selEnd = std::max(anchor, cursor);

  // Autofill selection: extends from (or contains) the cursor position
  // to the end of the line — the browser selected the suggestion text.
  if (selStart >= cursor || (selStart < cursor && selEnd > cursor)) {
    // Distinguish from multiline AI ghost text: selection must not
    // contain a newline character.
    const auto &text = surrounding.text();
    size_t p = text.find('\n', static_cast<size_t>(selStart));
    return p == std::string::npos || p >= static_cast<size_t>(selEnd);
  }
  return false;
}

SKeyOutputMode SKeyState::effectiveMode() const {
  // Cache result to avoid calling detectAutoMode() (which scans /proc via
  // isChromiumBasedApp) multiple times per keystroke.
  if (modeCacheValid_) {
    return cachedMode_;
  }

  const_cast<SKeyState *>(this)->refreshAppMode();

  // Address bar output mode overrides the general and per-application modes.
  // NoVietnamese is handled as a pass-through in keyEvent().
  if (inChromiumAddressBar()) {
    switch (engine_->config().chromiumAddressBarMode.value()) {
    case SKeyChromiumAddressBarMode::Auto:
      break; // fall through to normal Auto detection below
    case SKeyChromiumAddressBarMode::Uinput:
      cachedMode_ = SKeyOutputMode::Uinput;
      modeCacheValid_ = true;
      return cachedMode_;
    case SKeyChromiumAddressBarMode::SurroundingText:
      cachedMode_ = SKeyOutputMode::SurroundingText;
      modeCacheValid_ = true;
      return cachedMode_;
    case SKeyChromiumAddressBarMode::Preedit:
      cachedMode_ = SKeyOutputMode::Preedit;
      modeCacheValid_ = true;
      return cachedMode_;
    case SKeyChromiumAddressBarMode::NoVietnamese:
      break;
    }
  }

  auto resolved = hasAppModeOverride_ ? appModeOverride_
                                      : engine_->config().outputMode.value();

  if (resolved == SKeyOutputMode::Auto) {
    cachedMode_ = detectAutoMode();
  } else {
    cachedMode_ = resolved;
  }
  modeCacheValid_ = true;
  return cachedMode_;
}

bool SKeyState::useSurroundingText() const {
  auto mode = effectiveMode();
  return mode == SKeyOutputMode::SurroundingText ||
         mode == SKeyOutputMode::Uinput;
}

bool SKeyState::useUinputMode() const {
  return effectiveMode() == SKeyOutputMode::Uinput;
}

// Content-hint capability bits that indicate a real, trusted editor in a
// Chromium-family app (SpellCheck, Alpha, ...).  UppercaseWords (bit 19) is
// deliberately excluded: Google Sheets adds it on re-focus but still needs
// Uinput, while Facebook chat adds SpellCheck and works with SurroundingText.
static constexpr uint64_t kChromiumStrongHints =
    (1ULL << 3)  |  // Password
    (1ULL << 7)  |  // Email
    (1ULL << 8)  |  // Digit
    (1ULL << 9)  |  // Uppercase
    (1ULL << 10) |  // Lowercase
    (1ULL << 14) |  // Number
    (1ULL << 16) |  // SpellCheck
    (1ULL << 17) |  // NoSpellCheck
    (1ULL << 18) |  // WordCompletion
    (1ULL << 20) |  // UppercaseSentences
    (1ULL << 21) |  // Alpha
    (1ULL << 22);   // Name

/// How long a bare-caps decision stays deferred (microseconds).  The window
/// only needs to cover focus → click → first keystroke: either the caps get
/// updated (some apps re-sync their text-input state) or the AT-SPI2 focus
/// event lands and provides the web-editor signal (see a11yFreshWebEditor).
static constexpr uint64_t kBareCapsDecisionWindowUsec = 2000000; // 2s

bool SKeyState::a11yFreshWebEditor() const {
  auto *mon = engine_->a11yMonitor();
  if (!mon || !mon->isRunning())
    return false;
  // Snapshot must be recent: the focus event fires on click, and the mode
  // decision is re-evaluated at the next word boundary.
  if (!mon->isFocusSnapshotFresh(5000000)) // 5s
    return false;
  if (!mon->isWebContentFocused())
    return false;
  switch (mon->focusRole()) {
  case 61: // ATSPI_ROLE_TEXT
  case 73: // ATSPI_ROLE_PARAGRAPH
  case 79: // ATSPI_ROLE_ENTRY
  case 85: // ATSPI_ROLE_SECTION
  case 94: // ATSPI_ROLE_DOCUMENT_TEXT
  case 95: // ATSPI_ROLE_DOCUMENT_WEB
    return true;
  default:
    return false;
  }
}

SKeyOutputMode SKeyState::detectAutoMode() const {
  // Runtime override: if the surrounding text API was verified as
  // non-functional during a previous replacement attempt (cache invalid),
  // stick with Uinput for this IC — the per-replacement fallback cannot
  // be verified and corrupts text in apps that drop forwarded keys.
  if (surroundingTextFailed_) {
    SKEY_DEBUG() << "Auto: surrounding text previously failed → Uinput";
    return SKeyOutputMode::Uinput;
  }

  // Sticky Uinput: Chromium browsers (e.g. Google Sheets) may initially
  // report bare caps (0x72), then add content hints on re-focus without
  // actually providing a working SurroundingText editor.
  //
  // Exception: a fresh AT-SPI2 focus snapshot of a text-entry inside a web
  // document means the user just clicked a real editor (Facebook chat,
  // comments, web forms) — Chrome's caps are stale there, so bypass sticky.
  if (chromiumBareCapsUinput_) {
    if (!a11yFreshWebEditor()) {
      SKEY_DEBUG() << "Auto: sticky bare caps → Uinput";
      return SKeyOutputMode::Uinput;
    }
    SKEY_DEBUG() << "Auto: sticky bypassed by fresh web-editor focus";
  }

  auto caps = ic_->capabilityFlags();

  if (!caps.test(CapabilityFlag::SurroundingText)) {
    SKEY_DEBUG() << "Auto: no SurroundingText cap → Uinput";
    return SKeyOutputMode::Uinput;
  }

  // Terminal apps (Konsole, Alacritty, etc.) have their own internal
  // buffer — SurroundingText API doesn't sync correctly, so Uinput
  // raw key pass-through works better.
  if (caps.test(CapabilityFlag::Terminal) || isTerminalApp(ic_->program())) {
    SKEY_DEBUG() << "Auto: terminal app → Uinput";
    return SKeyOutputMode::Uinput;
  }

  // Chromium address bar always needs Uinput — the omnibox autocomplete
  // races with SurroundingText replacements, corrupting text.
  if (inChromiumAddressBar() && caps.test(CapabilityFlag::SurroundingText)) {
    SKEY_DEBUG() << "Auto: address bar → Uinput";
    return SKeyOutputMode::Uinput;
  }

  // Chromium-family apps (Electron + full browsers) with truly bare caps
  // (no content hints whatsoever) → Uinput.  Any content hint (including
  // UppercaseWords) indicates a real editor — SurroundingText.
  //
  // Bare caps at window focus are often stale: Chrome does NOT push updated
  // content type when focus moves within the page — it only re-syncs caps
  // when the text input is re-entered (an IC focus cycle, e.g. a second
  // click).  Google Sheets keeps bare caps (0x72) while typing → Uinput +
  // sticky flag.  Facebook chat shows 0x72 at window focus and 0x90072
  // (SpellCheck + UppercaseWords) only after the IC re-syncs → should be
  // SurroundingText.  The decision is therefore deferred and re-evaluated
  // at word boundaries (see keyEvent), where the AT-SPI2 web-editor focus
  // event (a11yFreshWebEditor) supplies the missing signal immediately.
  if (caps.test(CapabilityFlag::SurroundingText) && isChromiumCached() &&
      !caps.test(CapabilityFlag::Url)) {
    static constexpr uint64_t kContentHints =
        (1ULL << 3)  |  // Password
        (1ULL << 7)  |  // Email
        (1ULL << 8)  |  // Digit
        (1ULL << 9)  |  // Uppercase
        (1ULL << 10) |  // Lowercase
        (1ULL << 11) |  // NoAutoUpperCase
        (1ULL << 13) |  // Dialable
        (1ULL << 14) |  // Number
        (1ULL << 15) |  // NoOnScreenKeyboard
        (1ULL << 16) |  // SpellCheck
        (1ULL << 17) |  // NoSpellCheck
        (1ULL << 18) |  // WordCompletion
        (1ULL << 19) |  // UppercaseWords
        (1ULL << 20) |  // UppercaseSentences
        (1ULL << 21) |  // Alpha
        (1ULL << 22);    // Name
    CapabilityFlags contentHints(kContentHints);
    if (!(caps & contentHints)) {
      // Fresh AT-SPI2 web-editor focus (Facebook chat) wins over bare caps:
      // Chrome fires the a11y focus event immediately on click, but its
      // content-type caps may stay bare until a text-input re-sync.
      if (a11yFreshWebEditor()) {
        modeDecisionPending_ = false;
        SKEY_DEBUG() << "Auto: bare caps + fresh web-editor focus → "
                        "SurroundingText";
        return SKeyOutputMode::SurroundingText;
      }
      // First sighting: defer instead of locking in.  If the deadline has
      // already passed with caps still bare (Google Sheets), lock in the
      // sticky Uinput flag now.
      if (modeDecisionPending_ &&
          now(CLOCK_MONOTONIC) >= modeDecisionDeadlineUsec_) {
        modeDecisionPending_ = false;
        chromiumBareCapsUinput_ = true;
        engine_->chromiumBareCapsProgram_ = ic_->program();
        engine_->chromiumHadBareCaps_ = true;
        SKEY_DEBUG() << "Auto: bare caps confirmed after deferral → sticky Uinput";
      } else if (!modeDecisionPending_) {
        modeDecisionPending_ = true;
        modeDecisionDeadlineUsec_ =
            now(CLOCK_MONOTONIC) + kBareCapsDecisionWindowUsec;
        SKEY_DEBUG() << "Auto: bare caps → Uinput (deferred, caps=0x"
                     << std::hex
                     << static_cast<uint64_t>(caps.toInteger()) << std::dec
                     << ")";
      }
      return SKeyOutputMode::Uinput;
    }
  }

  // A deferred decision can now be tested against updated caps: strong
  // hints (SpellCheck, ...) mean a real editor — Facebook chat — upgrade
  // to SurroundingText.  Weak hints only (UppercaseWords) match the Google
  // Sheets re-focus pattern — lock in Uinput.
  if (modeDecisionPending_) {
    modeDecisionPending_ = false;
    CapabilityFlags strong(kChromiumStrongHints);
    if (caps & strong) {
      SKEY_DEBUG() << "Auto: deferred decision → SurroundingText (caps=0x"
                   << std::hex
                   << static_cast<uint64_t>(caps.toInteger()) << std::dec
                   << ")";
    } else {
      chromiumBareCapsUinput_ = true;
      engine_->chromiumBareCapsProgram_ = ic_->program();
      engine_->chromiumHadBareCaps_ = true;
      SKEY_DEBUG() << "Auto: deferred decision → sticky Uinput (weak hints only)";
      return SKeyOutputMode::Uinput;
    }
  }

  SKEY_DEBUG() << "Auto: SurroundingText cap → SurroundingText";
  return SKeyOutputMode::SurroundingText;
}

bool SKeyState::canEditWithSurroundingText() const {
  return useSurroundingText() &&
         ic_->capabilityFlags().test(CapabilityFlag::SurroundingText);
}

bool SKeyState::isChromiumCached() const {
  if (cachedIsChromium_ < 0) {
    cachedIsChromium_ = isChromiumBasedApp(ic_->program()) ? 1 : 0;
  }
  return cachedIsChromium_ == 1;
}

bool SKeyState::isFirefoxOrSnap() const {
  if (cachedIsFirefoxOrSnap_ >= 0) {
    return cachedIsFirefoxOrSnap_ == 1;
  }
  const std::string &prog = ic_->program();
  // Firefox program name (native or Snap)
  if (prog.find("firefox") != std::string::npos) {
    cachedIsFirefoxOrSnap_ = 1;
    return true;
  }
  // Detect Snap-packaged apps: scan /proc for the process and check
  // whether its binary lives under /snap/.
  if (!prog.empty()) {
    DIR *dir = opendir("/proc");
    if (dir) {
      struct dirent *entry;
      while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR || !isdigit(entry->d_name[0]))
          continue;
        std::string commPath = "/proc/" + std::string(entry->d_name) + "/comm";
        std::ifstream commFile(commPath);
        if (!commFile.is_open())
          continue;
        std::string comm;
        std::getline(commFile, comm);
        if (comm == prog || prog.compare(0, 15, comm) == 0 ||
            comm.compare(0, 15, prog) == 0) {
          // Found matching process — check exe path for /snap/
          std::string exePath =
              "/proc/" + std::string(entry->d_name) + "/exe";
          char buf[4096];
          ssize_t len = readlink(exePath.c_str(), buf, sizeof(buf) - 1);
          if (len > 0) {
            buf[len] = '\0';
            std::string exe(buf);
            if (exe.find("/snap/") != std::string::npos) {
              closedir(dir);
              cachedIsFirefoxOrSnap_ = 1;
              return true;
            }
          }
          // Also check via /proc/PID/maps (some Snap apps use
          // symlinked launchers where exe doesn't contain /snap/)
          std::string mapsPath =
              "/proc/" + std::string(entry->d_name) + "/maps";
          std::ifstream mapsFile(mapsPath);
          if (mapsFile.is_open()) {
            std::string line;
            if (std::getline(mapsFile, line) &&
                line.find("/snap/") != std::string::npos) {
              closedir(dir);
              cachedIsFirefoxOrSnap_ = 1;
              return true;
            }
          }
          break; // found matching process but not snap — done
        }
      }
      closedir(dir);
    }
  }
  cachedIsFirefoxOrSnap_ = 0;
  return false;
}

bool SKeyState::useNativeSurroundingApi() const {
  // Single effectiveMode() call — avoids double-evaluating detectAutoMode()
  // (which scans /proc via isChromiumBasedApp) on every keystroke.
  auto mode = effectiveMode();
  return mode == SKeyOutputMode::SurroundingText &&
         ic_->capabilityFlags().test(CapabilityFlag::SurroundingText);
}

bool SKeyState::isWayland() const {
  // ic_->display() returns "wayland:" on native Wayland apps,
  // "x11:" on X11 or XWayland apps.
  const auto display = ic_->display();
  if (display.find("wayland") != std::string::npos) {
    return true;
  }
  // XWayland apps report display="x11:".  Even though they run under
  // a Wayland compositor, the X11 protocol bridge adds enough latency
  // (especially through Wine/Proton translation layers) that Wayland
  // timing constants are too aggressive.  Treat X11-frontend apps as
  // X11 for timing purposes regardless of the underlying compositor.
  return false;
}

const UinputTiming &SKeyState::uinputTiming() const {
  return isWayland() ? kUinputTimingWayland : kUinputTimingX11;
}

bool SKeyState::useHiddenComposition() const { return false; }

void SKeyState::activate() {
  // Re-sync input method from config (handles config changes at runtime)
  auto &cfg = engine_->config();
  skey::InputMethod im = skey::InputMethod::Telex;
  if (*cfg.inputMethod == SKeyInputMethod::VNI) {
    im = skey::InputMethod::VNI;
  }
  // Charset — SKeyCharset and Charset enums share the same order
  charset_ = static_cast<skey::Charset>(*cfg.charset);

  viet_.setShortW(*cfg.shortW);
  viet_.setBracketUO(*cfg.bracketUO);
  viet_.setMethod(im);
  viet_.setFreeMarking(*cfg.freeMarking);
  viet_.setAutoRestore(*cfg.autoRestore);
  viet_.setDict(*cfg.dict);
  loadUserDict();

  // Reactivate after spurious cycle: cancel the genuine-loss timer.
  if (addrBarExpectCycle_) {
    SKEY_DEBUG() << "Activate: spurious cycle, cancel loss timer";
    addrBarCycleTimer_.reset();
  } else {
    // Spurious-cycle detection: if preeditWasPending_ is set and the
    // activating program matches, the same IC is being reactivated —
    // the app auto-committed on focus loss (e.g., LibreOffice).
    // Don't double-commit; just clean up the engine entry.
    auto it = engine_->pendingPreedits_.find(ic_->program());
    if (preeditWasPending_ && preeditPendingProgram_ == ic_->program()) {
      SKEY_DEBUG() << "Activate: spurious cycle for '"
                   << ic_->program() << "', discarding saved preedit";
      if (it != engine_->pendingPreedits_.end()) {
        engine_->pendingPreedits_.erase(it);
      }
    } else if (it != engine_->pendingPreedits_.end()) {
      // Genuine return — the IC was destroyed and recreated, or the
      // activating program differs from the one that saved the text.
      SKEY_DEBUG() << "Activate: committing saved preedit '"
                   << it->second << "' for program '" << ic_->program()
                   << "'";
      commitText(it->second);
      engine_->pendingPreedits_.erase(it);
      ic_->updatePreedit();
    }
    preeditWasPending_ = false;
    viet_.reset();
    committedLen_ = 0;
    surroundingTextFailed_ = false; // fresh focus, re-verify

    // Detect spurious focus cycles that arrived when addrBarExpectCycle_
    // was not armed (e.g. asynchronous omnibox updates).  If reactivation
    // happens within 500ms of deactivate in the same Chromium address bar,
    // preserve first-word/space tracking — otherwise the next replacement
    // gets fullReplace treatment and deletes text before the cursor.
    bool spuriousCycle = inChromiumAddressBar() && lastDeactivateTime_ > 0 &&
                         (now(CLOCK_MONOTONIC) - lastDeactivateTime_) < 500000;

    if (!spuriousCycle) {
      addrBarIsFirstWord_ = true;
      addrBarHadSpace_ = false;
    } else {
      SKEY_DEBUG() << "Activate: spurious cycle (unarmed), preserving"
                   << " firstWord=" << addrBarIsFirstWord_
                   << " hadSpace=" << addrBarHadSpace_;
    }
    addrBarHadFirstWord_ = false;
    addrBarDidFullReplace_ = false;
    addrBarKeepState_ = false;
    addrBarPrevCommittedLen_ = 0;
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
      // Normalize: strip quotes that safeSaveAsIni adds for values
      // containing spaces (e.g. "Surrounding Text" → Surrounding Text).
      std::string modeStr = *val;
      if (modeStr.size() >= 2 && modeStr.front() == '"' &&
          modeStr.back() == '"')
        modeStr = modeStr.substr(1, modeStr.size() - 2);

      if (modeStr == "Excluded") {
        appExcluded_ = true;
      } else {
        SKeyOutputMode savedMode = engine_->config().outputMode.value();
        if (modeStr == "Preedit")
          savedMode = SKeyOutputMode::Preedit;
        else if (modeStr == "SurroundingTextSlow" ||
                 modeStr == "Surrounding Text")
          savedMode = SKeyOutputMode::SurroundingText;
        else if (modeStr == "Uinput")
          savedMode = SKeyOutputMode::Uinput;
        else if (modeStr == "SurroundingText")
          savedMode = SKeyOutputMode::SurroundingText;
        else if (modeStr == "Auto")
          savedMode = SKeyOutputMode::Auto;
        appModeOverride_ = savedMode;
        hasAppModeOverride_ = true;
      }
    }
  }

  // Invalidate mode cache — new focus means caps/program may have changed.
  chromiumBareCapsUinput_ = false;
  modeCacheValid_ = false;
  cachedIsChromium_ = -1;
  cachedIsFirefoxOrSnap_ = -1;

  auto caps = ic_->capabilityFlags();

  // Engine-level sticky Uinput: if a previous IC for this Chromium program
  // reported bare caps (0x72), keep Uinput for subsequent ICs of the same
  // program — but only when the current caps lack "strong" content hints
  // (Alpha, SpellCheck, etc.) that indicate a real editor like Facebook chat.
  if (engine_->chromiumHadBareCaps_ &&
      ic_->program() == engine_->chromiumBareCapsProgram_ &&
      !ic_->program().empty() && isChromiumCached()) {
    CapabilityFlags strong(kChromiumStrongHints);
    if (!(caps & strong)) {
      chromiumBareCapsUinput_ = true;
      SKEY_DEBUG() << "Activate: engine sticky bare caps → Uinput";
    }
  }
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
               << " frontend=" << ic_->display()
               << " display=" << ic_->display() << " wayland=" << isWayland()
               << " app=" << ic_->program() << " caps=0x" << std::hex
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
  // flags bit 0: send Escape before BS (deprecated — autocomplete is now
  //   handled via extra BS when isAutofillCertain() detects a selection).
  // The server detects v1 vs v2 by message size for backward compatibility.
  int32_t count32 = count;
  uint32_t textLen = static_cast<uint32_t>(text.size());
  std::vector<char> msg(sizeof(int32_t) + sizeof(uint32_t) * 2 + textLen);
  memcpy(msg.data(), &count32, sizeof(count32));
  memcpy(msg.data() + sizeof(count32), &flags, sizeof(flags));
  memcpy(msg.data() + sizeof(count32) + sizeof(flags), &textLen,
         sizeof(textLen));
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
  // Track injected BS that will loop back through fcitx5 — used to
  // swallow late loopbacks instead of mistaking them for user backspaces.
  uinputBsOutstanding_ += count;
  SKEY_DEBUG() << "Uinput: sent BS=" << count
               << (textLen > 0 ? " +text='" + text + "'" : "");
  // When text is included, the server types it via Ctrl+Shift+U hex.
  // Those keystrokes loop back through fcitx5; enable passthrough so
  // they reach the app unmodified.  Window is sized per character
  // (roughly 15ms per char for Ctrl+Shift+U typing + latency).
  if (textLen > 0) {
    auto &timing = uinputTiming();
    uinputPassthroughUntil_ =
        now(CLOCK_MONOTONIC) +
        std::max(timing.passthroughMinUsec,
                 timing.passthroughBaseUsec +
                     static_cast<uint64_t>(textLen) * timing.perCharUsec);
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

    // Escape sent by our own uinput server (flags=1) must pass
    // through to Chrome — don't filter it.  It dismisses Chrome
    // inline autocomplete without deleting any characters, which
    // extra BS would incorrectly do.
    if (sym == FcitxKey_Escape) {
      SKEY_DEBUG() << "Uinput: pass Escape to app (autocomplete dismiss)";
      return true; // pass through (no filterAndAccept)
    }
    if (inChromiumAddressBar() && addrBarLastTriggerKey_ != 0 &&
        now(CLOCK_MONOTONIC) < addrBarTriggerDeadline_ &&
        sym == static_cast<uint32_t>(addrBarLastTriggerKey_)) {
      SKEY_DEBUG() << "Uinput: drop re-delivered trigger key 0x" << std::hex
                   << sym;
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

  // Count BS events.
  // N real BS pass through to the app (character deletion).
  // The (N+1)-th BS is the sync anchor — it's consumed here.
  if (seenUinputBackspaces_ < expectedUinputBackspaces_) {
    ++seenUinputBackspaces_;
    if (uinputBsOutstanding_ > 0)
      --uinputBsOutstanding_;
    if (committedLen_ > 0) {
      committedLen_--;
    }
    SKEY_DEBUG() << "Uinput: pass BS " << seenUinputBackspaces_ << "/"
                 << expectedUinputBackspaces_;
    if (isFirefoxOrSnap()) {
      uinputKeyForwarded_ = true;
    }
    return true; // forward real BS to app
  }

  // ── Sync BS arrived ──
  // The extra BS (+1 beyond real deletions) acts as a sync anchor.
  // By the time it arrives back at fcitx5, X11 has serialized all N real
  // BS to the app.  Consume it, adaptive sleep, commit synchronously.
  // Address bar uses its own conservative timing constants.
  keyEvent.filterAndAccept();
  uinputSafetyTimer_.reset();
  uinputSafetyRetried_ = false;
  if (uinputBsOutstanding_ > 0)
    --uinputBsOutstanding_;

  expectedUinputBackspaces_ = 0;
  seenUinputBackspaces_ = 0;

  std::string commitText = pendingUinputCommit_;
  pendingUinputCommit_.clear();

  uint64_t elapsed = now(CLOCK_MONOTONIC) - bsSentAt_;
  lastBsRoundTrip_ = elapsed;

  // Adaptive sleep via EWMA of measured round-trip times.
  auto &timing = uinputTiming();
  if (bsRtEwma_ == timing.bsRtInitialUsec || bsRtEwma_ == 0) {
    bsRtEwma_ = elapsed;
  } else {
    bsRtEwma_ = static_cast<uint64_t>(timing.bsRtEwmaAlpha * elapsed +
                                      (1.0 - timing.bsRtEwmaAlpha) * bsRtEwma_);
  }
  double multiplier;
  uint64_t minDelay, maxDelay;
  if (inChromiumAddressBar()) {
    multiplier = timing.addrBarBsRtMultiplier;
    minDelay = timing.addrBarCommitDelayMinUsec;
    maxDelay = timing.addrBarCommitDelayMaxUsec;
  } else {
    multiplier = timing.bsRtMultiplier;
    minDelay = timing.commitDelayMinUsec;
    maxDelay = timing.commitDelayMaxUsec;
    if (isChromiumCached()) {
      multiplier *= timing.chromiumDelayFactor;
      minDelay = static_cast<uint64_t>(minDelay * timing.chromiumDelayFactor);
      maxDelay = static_cast<uint64_t>(maxDelay * timing.chromiumDelayFactor);
    }
  }
  uint64_t sleepUsec = std::clamp(static_cast<uint64_t>(bsRtEwma_ * multiplier),
                                  minDelay, maxDelay);

  SKEY_DEBUG() << "Uinput: sync BS, RT " << (elapsed / 1000) << "ms (ewma "
               << (bsRtEwma_ / 1000) << "ms), sleep " << (sleepUsec / 1000)
               << "ms then commit '" << commitText << "'"
               << (inChromiumAddressBar() ? " [addrbar]" : "")
               << (isChromiumCached() ? " [chromium]" : "");

  usleep(sleepUsec);

  // ── Commit synchronously ──
  uinputDeleting_ = false;
  if (!commitText.empty()) {
    if (isFirefoxOrSnap()) {
      uinputKeyForwarded_ = true;
    }
    this->commitText(commitText);
  }
  if (uinputPendingFinalLen_ > 0) {
    committedLen_ = uinputPendingFinalLen_;
    uinputPendingFinalLen_ = 0;
  }
  // Never manually clear the trigger-key guard — let its deadline
  // auto-expire.  Chrome may re-deliver the trigger key after ANY
  // address bar replacement (both fullReplace first-word and normal
  // non-first-word), and clearing the guard too early lets the
  // re-delivered key through as a new keystroke, corrupting the text.
  // The 100ms deadline is long enough to catch re-delivery (~5ms) but
  // short enough to allow intentional double-presses (>150ms).
  if (addrBarDidFullReplace_) {
    addrBarDidFullReplace_ = false;
    addrBarKeepState_ = false;
    // Don't reset engine after replacement — preedit state is preserved
    // so subsequent keys extend the same word.
    committedLen_ = static_cast<int>(utf8::length(commitText));
    reclaimReady_ = false;
  }
  if (!bufferedUinputKeys_.empty()) {
    replayBufferedUinputKeys();
  }
  return true; // sync BS consumed
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
  lastDeactivateTime_ = now(CLOCK_MONOTONIC);
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
  // Firefox/Snap: preserve uinput state during spurious deactivate.
  if (uinputKeyForwarded_ && isFirefoxOrSnap()) {
    SKEY_DEBUG() << "Deactivate: Firefox/Snap guard hit, preserving state";
    uinputKeyForwarded_ = false;
    return;
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
          // Next word in a fresh context is the first word again
          // (e.g. new tab, new page) — fullReplace with autoRestore
          // should apply.
          addrBarHadFirstWord_ = false;
          // Only commit/flush if using non-uinput modes (preedit, etc.)
          // where the composition hasn't been committed yet.  In uinput
          // mode, replacements already committed via commitText() so
          // commitBuffer() here would double-commit.
          if (!useUinputMode()) {
            // Save preedit for restore on next activation (see reset()).
            if (!viet_.getComposed().empty() && !useSurroundingText()) {
              if (!preeditWasPending_) {
                preeditWasPending_ = true;
                preeditPendingProgram_ = ic_->program();
                engine_->pendingPreedits_[ic_->program()] =
                    viet_.getComposed();
              }
            }
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
  bsRtEwma_ = uinputTiming().bsRtInitialUsec;
  uinputCommitTimer_.reset();
  uinputSafetyTimer_.reset();
  uinputDeleting_ = false;

  // Save preedit for restore on next activation (see reset()).
  if (!viet_.getComposed().empty() && !useSurroundingText()) {
    if (!preeditWasPending_) {
      preeditWasPending_ = true;
      preeditPendingProgram_ = ic_->program();
      engine_->pendingPreedits_[ic_->program()] = viet_.getComposed();
      SKEY_DEBUG() << "Deactivate: saved preedit '"
                   << viet_.getComposed() << "' for program '"
                   << ic_->program() << "'";
    }
  }
  viet_.reset();
  committedLen_ = 0;
  clearLastWord();
  clearUI();
}

void SKeyState::reset() {
  SKEY_DEBUG() << "Reset: entered uinputFwd=" << uinputKeyForwarded_
               << " ffSnap=" << isFirefoxOrSnap() << " prog=" << ic_->program();
  if (addrBarExpectCycle_) {
    SKEY_DEBUG() << "Reset: expecting cycle, skip";
    return;
  }
  // Firefox/Snap apps in Uinput mode: fcitx5 calls reset() after
  // unfiltered keys.  Skip cleanup to preserve viet_ state for
  // multi-key composition.  Chromium/Electron apps are NOT affected
  // — they need the full cleanup (especially clearUI D-Bus).
  if (uinputKeyForwarded_ && isFirefoxOrSnap()) {
    SKEY_DEBUG() << "Reset: Firefox/Snap guard hit, preserving viet_ state";
    uinputKeyForwarded_ = false;
    return;
  }
  if (hasDeferredCommitPending()) {
    SKEY_DEBUG() << "Reset: keeping deferred commit";
  }
  // Save uncommitted preedit to engine (global, survives IC
  // destruction) and set spurious-cycle flag (per-IC).
  // commitString() during reset() is silently dropped on some
  // Wayland compositors (GNOME Mutter).
  if (!viet_.getComposed().empty() && !useSurroundingText()) {
    preeditWasPending_ = true;
    preeditPendingProgram_ = ic_->program();
    engine_->pendingPreedits_[ic_->program()] = viet_.getComposed();
    SKEY_DEBUG() << "Reset: saved preedit '" << viet_.getComposed()
                 << "' for program '" << ic_->program() << "'";
  }
  viet_.reset();
  bufferedUinputKeys_.clear();
  uinputCommitTimer_.reset();
  uinputSafetyTimer_.reset();
  uinputDeleting_ = false;
  // Any injected BS sent to the previous focus can no longer be
  // distinguished from real keypresses — drop the outstanding count.
  uinputBsOutstanding_ = 0;
  uinputSafetyRetried_ = false;
  if (!hasDeferredCommitPending()) {
    committedLen_ = 0;
  }
  modeCacheValid_ = false;
  cachedIsChromium_ = -1;
  cachedIsFirefoxOrSnap_ = -1;
  clearLastWord();
  clearUI();
}

void SKeyState::keyEvent(KeyEvent &keyEvent) {
  if (keyEvent.isRelease()) {
    return;
  }

  // Refresh per-app mode in case IC is shared across apps
  refreshAppMode();

  // Deferred mode decision (bare Chromium caps): re-evaluate at word
  // boundaries so a content-type update that arrived after focus (e.g.
  // clicking into the Facebook chat editor) can upgrade Uinput →
  // SurroundingText before the next word starts.  Mid-word the cached
  // decision is kept so the composition path never switches half-way.
  // A fresh AT-SPI2 web-editor snapshot also forces re-evaluation — the
  // a11y focus event fires on click, but may land after activate() already
  // cached a sticky-Uinput decision.
  if (viet_.getRawInput().empty() &&
      (modeDecisionPending_ || a11yFreshWebEditor())) {
    modeCacheValid_ = false;
  }

  // Track current word state so reclaim targets the right word
  // (not a stale previously-saved one after backspace).
  saveLastWord();

  // Clear backspace tracking when typing a non-BS key
  if (!keyEvent.key().check(FcitxKey_BackSpace)) {
    wordWasBackspaced_ = false;
  }

  // App excluded — pass all keys through, except backtick for menu
  if (appExcluded_ && !modeMenuActive_) {
    if (keyEvent.key().check(engine_->modeMenuKey()) &&
        viet_.getRawInput().empty()) {
      showModeMenu();
      keyEvent.filterAndAccept();
    }
    return;
  }

  // Password fields & lock/login screens (incl. sudo password prompts
  // in terminals): pass keys through unmodified.
  // Four detection paths:
  //   1. CapabilityFlag::PasswordOrSensitive (Wayland)
  //   2. CapabilityFlag::Password (X11, GTK/Qt password fields)
  //   3. AT-SPI2 accessibility monitor (X11 fallback)
  //   4. Lock screen program name (kscreenlocker_greet, i3lock, etc.)
  //
  // Terminal password prompts (sudo, ssh, passwd) on X11 cannot be
  // detected automatically — the terminal emulator does not change
  // capability flags when the shell reads a password.  Toggle fcitx5
  // off (Ctrl+Space) before typing passwords in terminals on X11.
  if (!modeMenuActive_ &&
      (ic_->capabilityFlags().test(CapabilityFlag::PasswordOrSensitive) ||
       ic_->capabilityFlags().test(CapabilityFlag::Password) ||
       (engine_->a11yMonitor() &&
        engine_->a11yMonitor()->isPasswordFocused()) ||
       programIsLockScreen(ic_->program()))) {
    return;
  }

  // Chromium address bar set to "No Vietnamese" — pass keys through so the
  // user types plain ASCII in the URL bar (web content is unaffected).
  if (!modeMenuActive_ &&
      engine_->config().chromiumAddressBarMode.value() ==
          SKeyChromiumAddressBarMode::NoVietnamese &&
      inChromiumAddressBar() && !keyEvent.key().check(engine_->modeMenuKey())) {
    return;
  }

  if (handlePendingUinputBackspace(keyEvent)) {
    return;
  }

  // Late uinput BS loopbacks — BS we injected that arrive after the
  // deletion window closed (slow apps).  Swallow them: treating them as
  // fresh user backspaces would pop composition chars and forward stray
  // deletions to the app.
  if (uinputBsOutstanding_ > 0 &&
      keyEvent.key().check(FcitxKey_BackSpace)) {
    --uinputBsOutstanding_;
    SKEY_DEBUG() << "Uinput: swallow late BS (" << uinputBsOutstanding_
                 << " outstanding)";
    keyEvent.filterAndAccept();
    return;
  }

  // Drop re-delivered trigger key after address bar replacement.
  // Chrome's spurious focus cycles cause X11 to re-send the key that
  // triggered the last replacement, even after uinput deletion completed.
  // We use a 200ms deadline so the guard doesn't stay active forever.
  if (inChromiumAddressBar() && addrBarLastTriggerKey_ != 0 &&
      now(CLOCK_MONOTONIC) < addrBarTriggerDeadline_) {
    if (keyEvent.key().sym() == static_cast<uint32_t>(addrBarLastTriggerKey_)) {
      SKEY_DEBUG() << "AddrBar: drop re-delivered trigger key 0x" << std::hex
                   << keyEvent.key().sym();
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
    else if (sym == FcitxKey_5 || sym == FcitxKey_KP_5)
      choice = 5;

    if (modeMenuForAddressBar_) {
      SKeyChromiumAddressBarMode newMode;
      switch (choice) {
      case 1:
        newMode = SKeyChromiumAddressBarMode::Auto;
        break;
      case 2:
        newMode = SKeyChromiumAddressBarMode::Uinput;
        break;
      case 3:
        newMode = SKeyChromiumAddressBarMode::SurroundingText;
        break;
      case 4:
        newMode = SKeyChromiumAddressBarMode::Preedit;
        break;
      case 5:
        newMode = SKeyChromiumAddressBarMode::NoVietnamese;
        break;
      default:
        return; // unrecognized — dismiss below
      }
      engine_->setChromiumAddressBarMode(newMode);
      SKEY_INFO() << "Address bar mode switched";
      dismissModeMenu();
      keyEvent.filterAndAccept();
      return;
    } else if (choice > 0 && choice <= 4) {
      SKeyOutputMode newMode;
      switch (choice) {
      case 1:
        newMode = SKeyOutputMode::Auto;
        break;
      case 2:
        newMode = SKeyOutputMode::Uinput;
        break;
      case 3:
        newMode = SKeyOutputMode::SurroundingText;
        break;
      case 4:
        newMode = SKeyOutputMode::Preedit;
        break;
      default:
        break; // unreachable
      }
      appExcluded_ = false;
      engine_->saveAppExcluded(ic_->program(), false);
      appModeOverride_ = newMode;
      hasAppModeOverride_ = true;
      modeCacheValid_ = false;
      engine_->saveAppMode(ic_->program(), newMode);
      SKEY_INFO() << "Mode switched to " << outputModeName(newMode);
      dismissModeMenu();
      keyEvent.filterAndAccept();
      return;
    } else if (choice == 5) {
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
  if (key.check(engine_->modeMenuKey()) && viet_.getRawInput().empty() &&
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
      // Auto-restore: when enabled, restore non-Vietnamese words
      {
        std::string preRestore = viet_.getComposed();
        viet_.autoRestore();
        std::string postRestore = viet_.getComposed();
        if (preRestore != postRestore && useSurroundingText()) {
          SKEY_DEBUG() << "AutoRestore: '" << preRestore
                       << "' -> '" << postRestore << "'";
          surroundingCommit(preRestore, postRestore);
        }
      }
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
    if (inChromiumAddressBar()) {
      addrBarHadFirstWord_ = false;
      addrBarDidFullReplace_ = false;
      addrBarKeepState_ = false;
      committedLen_ = 0;
    }
    return;
  }

  // Handle Backspace while composing
  if (key.check(FcitxKey_BackSpace) && !viet_.getRawInput().empty()) {
    // Chromium address bar: pass raw BS through to Chrome (X11) instead
    // of sending forwardKey via D-Bus (which triggers focus changes).
    // Just update bamboo state and let the keystroke reach the app.
    if (inChromiumAddressBar()) {
      if (addrBarDidFullReplace_) {
        addrBarDidFullReplace_ = false;
        addrBarKeepState_ = false;
        viet_.reset();
        committedLen_ = 0;
        // FullReplace word was deleted; re-enable for retype so
        // Chrome autocomplete doesn't consume the first BS.
        addrBarHadFirstWord_ = false;
        return;
      }
      // After a keep-state fullReplace (ASCII→VN transform like
      // "e"+"f"→"è" or "cha"+"f"→"chà"), the composed text was
      // committed but the engine kept tracking raw input.
      // - Single-char composed: one BS from Chrome deletes the
      //   entire character — reset the engine.
      // - Multi-char composed: BS only deletes the last character
      //   (e.g. 'à' from "chà" leaving "ch") — backspace through.
      if (addrBarKeepState_) {
        addrBarKeepState_ = false;
        int compLen = static_cast<int>(utf8::length(viet_.getComposed()));
        if (compLen <= 1) {
          viet_.reset();
          committedLen_ = 0;
          // Re-enable fullReplace for the next word.  The keep-state
          // word was committed and is now deleted; Chrome may re-show
          // autocomplete when the user retypes.  The addrBarHadSpace_
          // guard in fullReplace prevents extra BS from damaging
          // multi-word text when surrounding text is unavailable.
          addrBarHadFirstWord_ = false;
        } else {
          viet_.backspace();
          committedLen_ = compLen - 1;
          addrBarExpectCycle_ = true;
          if (viet_.getRawInput().empty()) {
            addrBarIsFirstWord_ = true;
          }
        }
        return;
      }
      viet_.backspace();
      committedLen_ = viet_.getRawInput().empty()
                          ? 0
                          : static_cast<int>(utf8::length(viet_.getComposed()));
      // Re-arm cycle protection and, only when no space has been
      // typed yet, the first-word flag.  After a space the next word
      // won't trigger Chrome autocomplete, so fullReplace is not needed
      // and would damage text before the cursor.
      addrBarExpectCycle_ = true;
      if (viet_.getRawInput().empty() && !addrBarHadSpace_) {
        addrBarIsFirstWord_ = true;
      }
      SKEY_DEBUG() << "AddrBar BS: rawInput='" << viet_.getRawInput()
                   << "' composed='" << viet_.getComposed()
                   << "' len=" << committedLen_;
      return; // pass raw BS through to Chrome
    }
    if (useSurroundingText()) {
      if (useUinputMode()) {
        // A replacement in flight cannot tell user BS apart from
        // injected-BS loopbacks — cancel the pending commit (the screen
        // keeps whatever deletions already arrived) and handle this BS
        // as a normal keystroke undo.
        if (uinputDeleting_) {
          SKEY_DEBUG() << "SurrBS: user BS during uinput replace — cancel";
          uinputSafetyTimer_.reset();
          uinputSafetyRetried_ = false;
          pendingUinputCommit_.clear();
          expectedUinputBackspaces_ = 0;
          seenUinputBackspaces_ = 0;
          uinputDeleting_ = false;
          // uinputBsOutstanding_ stays — late loopbacks get swallowed.
        }
        std::string oldComposed = viet_.getComposed();
        SKEY_DEBUG() << "SurrBS: uinput compose '" << oldComposed << "'";
        if (committedLen_ > 0) {
          committedLen_--;
          wordWasBackspaced_ = true; // user is deleting the word
        }
        // The raw BS passes through and the app deletes the last rendered
        // char.  Keep the engine composing — but when the recomposed text
        // differs from what the app will show (merged pairs: "đ" BS,
        // "ươ" BS), follow the app: feed the visible remainder as raw
        // input.  The engine round-trips precomposed Vietnamese, so no
        // injected-BS replacement is needed.
        viet_.backspace();
        std::string appText = oldComposed;
        if (!appText.empty()) {
          size_t last = appText.size() - 1;
          while (last > 0 && isUtf8ContinuationByte(appText[last])) --last;
          appText.resize(last);
        }
        if (viet_.getComposed() != appText) {
          SKEY_DEBUG() << "SurrBS: uinput follow app, raw='" << appText << "'";
          viet_.setRawInput(appText);
        }
        committedLen_ = viet_.getRawInput().empty()
                            ? 0
                            : static_cast<int>(utf8::length(viet_.getComposed()));
        SKEY_DEBUG() << "SurrBS: uinput compose -> '" << viet_.getComposed()
                     << "' len=" << committedLen_;
        return; // pass through raw BS (do NOT filter)
      }
      // Keep composing: pop the last raw char instead of resetting the
      // engine, so editing mid-word works (e.g. "hangh" BS → "hang",
      // then "f" → "hàng").  The app deletes the last rendered char;
      // when the recomposed text differs (merged pairs), follow the app
      // by feeding the visible remainder as raw input.
      std::string oldComposed = viet_.getComposed();
      SKEY_DEBUG() << "SurrBS compose: '" << oldComposed << "'";
      viet_.backspace();
      std::string appText = oldComposed;
      if (!appText.empty()) {
        size_t last = appText.size() - 1;
        while (last > 0 && isUtf8ContinuationByte(appText[last])) --last;
        appText.resize(last);
      }
      if (viet_.getComposed() != appText) {
        SKEY_DEBUG() << "SurrBS: follow app, raw='" << appText << "'";
        viet_.setRawInput(appText);
      }
      committedLen_ = viet_.getRawInput().empty()
                          ? 0
                          : static_cast<int>(utf8::length(viet_.getComposed()));
      SKEY_DEBUG() << "SurrBS compose -> '" << viet_.getComposed()
                   << "' len=" << committedLen_;
      if (useNativeSurroundingApi()) {
        const auto &surrounding = ic_->surroundingText();
        if (surrounding.isValid() &&
            surroundingCacheEndsWith(surrounding, oldComposed)) {
          ic_->deleteSurroundingText(-1, 1);
          if (ic_->surroundingText().isValid()) {
            ic_->surroundingText().deleteText(-1, 1);
          }
        } else {
          // Invalid or stale cache (e.g. Chromium after key re-delivery):
          // let the app delete the character itself.  Native deletes
          // against a stale cache remove the wrong characters.
          ic_->forwardKey(Key(FcitxKey_BackSpace));
        }
      } else {
        ic_->forwardKey(Key(FcitxKey_BackSpace));
      }
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
      // If we have a valid surrounding text with a selection, delete the
      // entire selection in one operation.
      const auto &surrounding = ic_->surroundingText();
      if (surrounding.isValid() &&
          surrounding.anchor() != surrounding.cursor()) {
        unsigned int selStart =
            std::min(surrounding.anchor(), surrounding.cursor());
        unsigned int selEnd =
            std::max(surrounding.anchor(), surrounding.cursor());
        unsigned int deleteSize = selEnd - selStart;
        SKEY_DEBUG() << "SurrBS: delete selection size=" << deleteSize
                     << " via forwardKey";
        // Forward a raw Backspace key so the app handles selection
        // deletion natively, then update local cache.
        ic_->forwardKey(Key(FcitxKey_BackSpace));
        if (ic_->surroundingText().isValid()) {
          ic_->surroundingText().deleteText(
              static_cast<int>(selStart) -
                  static_cast<int>(surrounding.cursor()),
              deleteSize);
        }
        committedLen_ = 0;
        clearLastWord();
        reclaimReady_ = false;
        keyEvent.filterAndAccept();
        return;
      }
      // When surrounding text cache is empty or has no cursor position
      // info (app hasn't sent meaningful surrounding text), pass the
      // raw Backspace through to the app so it can handle selection
      // deletion natively.  deleteSurroundingText(-1, 1) only deletes 1
      // character and cannot clear a multi-character selection.
      if ((!surrounding.isValid() || surrounding.cursor() == 0) &&
          committedLen_ <= 0) {
        SKEY_DEBUG() << "SurrBS: forwardKey (valid=" << surrounding.isValid()
                     << " cursor=" << surrounding.cursor() << ")";
        ic_->forwardKey(Key(FcitxKey_BackSpace));
        keyEvent.filterAndAccept();
        return;
      }
      if (committedLen_ > 0) {
        surroundingBackspace();
        keyEvent.filterAndAccept();
        return;
      }
      // No committed text — the character before the cursor may be the
      // separator (space) between the last word and the previous one.
      //
      // First BS at committedLen_ == 0: DO NOT delete yet.  Just enable
      // reclaim so a subsequent tone key can trigger tone editing (which
      // will delete the separator as part of the reclaim operation).
      //
      // Subsequent BS (committedLen_ == -1): delete into the previous
      // word — the user is intentionally removing characters.
      if (!lastRawInput_.empty()) {
        if (committedLen_ == 0) {
          // Delete the separator immediately AND enable reclaim for
          // potential retroactive tone editing on the next keystroke.
          ic_->deleteSurroundingText(-1, 1);
          if (ic_->surroundingText().isValid()) {
            ic_->surroundingText().deleteText(-1, 1);
          }
          reclaimReady_ = true;
          sepAlreadyDeleted_ = true;
          committedLen_ = -1; // sentinel: ready for reclaim or deletion
          SKEY_DEBUG() << "SurrBS: delete 1 (sep) + reclaim ready";
        } else if (committedLen_ == -1) {
          // Second+ call: deleting into the previous word
          reclaimReady_ = false;
          sepAlreadyDeleted_ = false;
          ic_->deleteSurroundingText(-1, 1);
          if (ic_->surroundingText().isValid()) {
            ic_->surroundingText().deleteText(-1, 1);
          }
          SKEY_DEBUG() << "SurrBS: delete 1 via surrounding text";
        }
        // else committedLen_ > 0: handled above (surroundingBackspace)
      } else {
        // No saved previous word — just delete the character.
        ic_->deleteSurroundingText(-1, 1);
        if (ic_->surroundingText().isValid()) {
          ic_->surroundingText().deleteText(-1, 1);
        }
        SKEY_DEBUG() << "SurrBS: delete 1 via surrounding text";
      }
      keyEvent.filterAndAccept();
      return;
    }
    // Non-native-surrounding path: pass through.  The app deletes the
    // character before the cursor itself (normally the separator the
    // engine just committed), so mark the separator as already deleted —
    // otherwise the tone-key reclaim path would delete a second character
    // via the surrounding-text API and desync the engine from the screen.
    if (!lastRawInput_.empty() && !wordWasBackspaced_) {
      reclaimReady_ = true;
      sepAlreadyDeleted_ = true;
    }
    // Re-arm cycle protection for address bar (see composing BS handler).
    if (inChromiumAddressBar()) {
      addrBarExpectCycle_ = true;
      // X11 Uinput: manually decrement committedLen_ (no SurroundingText).
      // The addrBarPrevCommittedLen_ snapshot in scheduleAddrBarReplacement
      // determines FullReplace safety — no complex delete-target logic needed.
      if (!isWayland() && committedLen_ > 0) {
        committedLen_--;
        wordWasBackspaced_ = true;
      }
      if (committedLen_ == 0) {
        committedLen_ = -1;
      } else if (committedLen_ == -1) {
        reclaimReady_ = false;
        wordWasBackspaced_ = true;
      }
    } else {
      // Non-Chromium: use sentinel to cancel reclaim on second idle BS
      // (mirrors the surrounding-text path's second-BS cancel and the
      // address bar sentinel above).  Without this, reclaim stays armed
      // forever and a new word starting with a tone key resurrects the
      // deleted word.
      if (committedLen_ == 0 && reclaimReady_) {
        // First idle BS at word boundary → sentinel
        committedLen_ = -1;
      } else if (committedLen_ == -1) {
        // Second idle BS → user is deleting, cancel reclaim
        reclaimReady_ = false;
        sepAlreadyDeleted_ = false;
        wordWasBackspaced_ = true;
        committedLen_ = 0;
      }
    }
    if (committedLen_ > 0) {
      committedLen_--;
      if (committedLen_ == 0) {
        wordWasBackspaced_ = true;
        addrBarHadFirstWord_ = false;
      }
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
      // ── Macro / Gõ tắt check ──
      if (engine_->config().enableMacro.value()) {
        std::string rawInput = viet_.getRawInput();
        std::string macroVal = engine_->lookupMacro(rawInput);
        if (!macroVal.empty()) {
          if (engine_->config().capitalizeMacro.value() && !rawInput.empty() &&
              std::isupper(static_cast<unsigned char>(rawInput[0]))) {
            macroVal[0] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(macroVal[0])));
          }
          SKEY_INFO() << "Macro: '" << rawInput << "' → '" << macroVal << "'";
          std::string oldComposed = viet_.getComposed();
          viet_.reset();
          committedLen_ = 0;
          if (useSurroundingText()) {
            surroundingCommit(oldComposed, macroVal);
          } else {
            clearUI();
            commitText(macroVal);
          }
          keyEvent.filterAndAccept();
          return;
        }
      }
      // ── End macro check ──
      // Auto-restore: when enabled, restore non-Vietnamese words
      // to their raw form at commit time (Unikey-style).
      bool autoRestored = false;
      {
        std::string preRestore = viet_.getComposed();
        viet_.autoRestore();
        std::string postRestore = viet_.getComposed();
        if (preRestore != postRestore && useSurroundingText()) {
          autoRestored = true;
          SKEY_DEBUG() << "AutoRestore: '" << preRestore
                       << "' -> '" << postRestore << "'";
          if (useUinputMode()) {
            int oldLen = static_cast<int>(utf8::length(preRestore));
            sendBackspaceUinput(oldLen + 1);
            expectedUinputBackspaces_ = oldLen;
            seenUinputBackspaces_ = 0;
            pendingUinputCommit_ = postRestore + "\n";
            uinputPendingFinalLen_ =
                static_cast<int>(utf8::length(postRestore));
            uinputDeleting_ = true;
            committedLen_ = uinputPendingFinalLen_;
            uinputSafetyTimer_ =
                engine_->instance()->eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC,
                    now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec,
                    0, [this](EventSourceTime *, uint64_t) {
                      SKEY_DEBUG()
                          << "AutoRestore: safety timeout, force commit";
                      uinputSafetyTimer_.reset();
                      uinputCommitTimer_.reset();
                      std::string text = std::move(pendingUinputCommit_);
                      pendingUinputCommit_.clear();
                      expectedUinputBackspaces_ = 0;
                      seenUinputBackspaces_ = 0;
                      uinputDeleting_ = false;
                      if (!text.empty())
                        this->commitText(text);
                      committedLen_ = uinputPendingFinalLen_;
                      uinputPendingFinalLen_ = 0;
                      if (!bufferedUinputKeys_.empty())
                        replayBufferedUinputKeys();
                      return true;
                    });
          } else {
            surroundingCommit(preRestore, postRestore);
          }
        }
      }
      saveLastWord();
      if (useSurroundingText()) {
        forceFlushDeferredCommit();
      } else {
        commitBuffer();
        clearUI();
      }
      viet_.reset();
      // When autoRestored, the uinput commit handler sets the final
      // committedLen_ asynchronously — don't race with 0 here.
      if (!autoRestored) {
        committedLen_ = 0;
      }
      if (autoRestored && useUinputMode()) {
        keyEvent.filterAndAccept();
      }
    }
    return;
  }

  // Handle Space
  if (key.check(FcitxKey_space)) {
    if (!viet_.getRawInput().empty()) {
      // ── Macro / Gõ tắt check ──
      if (engine_->config().enableMacro.value()) {
        std::string rawInput = viet_.getRawInput();
        std::string macroVal = engine_->lookupMacro(rawInput);
        if (!macroVal.empty()) {
          // Capitalize first letter if raw input started with uppercase
          if (engine_->config().capitalizeMacro.value() && !rawInput.empty() &&
              std::isupper(static_cast<unsigned char>(rawInput[0]))) {
            macroVal[0] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(macroVal[0])));
          }
          SKEY_INFO() << "Macro: '" << rawInput << "' → '" << macroVal << "'";
          std::string oldComposed = viet_.getComposed();
          viet_.reset();
          committedLen_ = 0;
          if (useSurroundingText()) {
            // Replace preedit text on screen with macro expansion
            surroundingCommit(oldComposed, macroVal + " ");
          } else {
            clearUI();
            commitText(macroVal);
            ic_->commitString(" ");
          }
          keyEvent.filterAndAccept();
          return;
        }
      }
      // ── End macro check ──
      // Auto-restore: when enabled, restore non-Vietnamese words
      // to their raw form at commit time (Unikey-style).
      bool autoRestored = false;
      {
        std::string preRestore = viet_.getComposed();
        viet_.autoRestore();
        std::string postRestore = viet_.getComposed();
        if (preRestore != postRestore && useSurroundingText()) {
          autoRestored = true;
          SKEY_DEBUG() << "AutoRestore: '" << preRestore
                       << "' -> '" << postRestore << "'";
          if (useUinputMode()) {
            // Full replacement via uinput BS + commit.
            // Include the space in the pending commit so it arrives
            // AFTER the BS events (avoids race with D-Bus commitString).
            int oldLen = static_cast<int>(utf8::length(preRestore));
            sendBackspaceUinput(oldLen + 1); // +1 sync BS
            expectedUinputBackspaces_ = oldLen;
            seenUinputBackspaces_ = 0;
            pendingUinputCommit_ = postRestore + " ";
            uinputPendingFinalLen_ =
                static_cast<int>(utf8::length(postRestore));
            uinputDeleting_ = true;
            committedLen_ = uinputPendingFinalLen_;
            uinputSafetyTimer_ =
                engine_->instance()->eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC,
                    now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec,
                    0, [this](EventSourceTime *, uint64_t) {
                      SKEY_DEBUG()
                          << "AutoRestore: safety timeout, force commit";
                      uinputSafetyTimer_.reset();
                      uinputCommitTimer_.reset();
                      std::string text = std::move(pendingUinputCommit_);
                      pendingUinputCommit_.clear();
                      expectedUinputBackspaces_ = 0;
                      seenUinputBackspaces_ = 0;
                      uinputDeleting_ = false;
                      if (!text.empty())
                        this->commitText(text);
                      committedLen_ = uinputPendingFinalLen_;
                      uinputPendingFinalLen_ = 0;
                      if (!bufferedUinputKeys_.empty())
                        replayBufferedUinputKeys();
                      return true;
                    });
          } else {
            surroundingCommit(preRestore, postRestore);
          }
        }
      }
      saveLastWord();
      if (useSurroundingText()) {
        bool hadDeferred = hasDeferredCommitPending();
        if (hadDeferred) {
          pendingFlushSuffix_ += " ";
        }
        forceFlushDeferredCommit();
        if (!hadDeferred && !(autoRestored && useUinputMode())) {
          ic_->commitString(" ");
        }
        keyEvent.filterAndAccept();
      } else {
        commitBuffer();
        clearUI();
      }
      viet_.reset();
      // When autoRestored, the uinput commit handler will set the final
      // committedLen_ (= uinputPendingFinalLen_) asynchronously.  Don't
      // clobber it with 0 here — that would race and could leave the
      // engine in an inconsistent state for the next word.
      if (!autoRestored) {
        committedLen_ = 0;
      }
      // After space, the next word is NOT the first word — autocomplete
      // won't trigger on multi-word text.
      addrBarIsFirstWord_ = false;
      addrBarHadSpace_ = true;
      addrBarHadFirstWord_ = true;
    } else if (reclaimReady_) {
      // Space typed after backspacing the separator but before a tone
      // key — user wants a new word, not to edit the previous word's
      // tone.  Cancel reclaim so subsequent tone keys start fresh.
      SKEY_DEBUG() << "Reclaim: cancelled by space";
      reclaimReady_ = false;
      sepAlreadyDeleted_ = false;
    }
    return;
  }

  // Handle Tab
  if (key.check(FcitxKey_Tab)) {
    if (!viet_.getRawInput().empty()) {
      // ── Macro / Gõ tắt check ──
      if (engine_->config().enableMacro.value()) {
        std::string rawInput = viet_.getRawInput();
        std::string macroVal = engine_->lookupMacro(rawInput);
        if (!macroVal.empty()) {
          if (engine_->config().capitalizeMacro.value() && !rawInput.empty() &&
              std::isupper(static_cast<unsigned char>(rawInput[0]))) {
            macroVal[0] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(macroVal[0])));
          }
          SKEY_INFO() << "Macro: '" << rawInput << "' → '" << macroVal << "'";
          std::string oldComposed = viet_.getComposed();
          viet_.reset();
          committedLen_ = 0;
          if (useSurroundingText()) {
            surroundingCommit(oldComposed, macroVal);
          } else {
            clearUI();
            commitText(macroVal);
          }
          keyEvent.filterAndAccept();
          return;
        }
      }
      // ── End macro check ──
      // Auto-restore: when enabled, restore non-Vietnamese words
      // to their raw form at commit time (Unikey-style).
      bool autoRestored = false;
      {
        std::string preRestore = viet_.getComposed();
        viet_.autoRestore();
        std::string postRestore = viet_.getComposed();
        if (preRestore != postRestore && useSurroundingText()) {
          autoRestored = true;
          SKEY_DEBUG() << "AutoRestore: '" << preRestore
                       << "' -> '" << postRestore << "'";
          if (useUinputMode()) {
            int oldLen = static_cast<int>(utf8::length(preRestore));
            sendBackspaceUinput(oldLen + 1);
            expectedUinputBackspaces_ = oldLen;
            seenUinputBackspaces_ = 0;
            pendingUinputCommit_ = postRestore + "\t";
            uinputPendingFinalLen_ =
                static_cast<int>(utf8::length(postRestore));
            uinputDeleting_ = true;
            committedLen_ = uinputPendingFinalLen_;
            uinputSafetyTimer_ =
                engine_->instance()->eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC,
                    now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec,
                    0, [this](EventSourceTime *, uint64_t) {
                      SKEY_DEBUG()
                          << "AutoRestore: safety timeout, force commit";
                      uinputSafetyTimer_.reset();
                      uinputCommitTimer_.reset();
                      std::string text = std::move(pendingUinputCommit_);
                      pendingUinputCommit_.clear();
                      expectedUinputBackspaces_ = 0;
                      seenUinputBackspaces_ = 0;
                      uinputDeleting_ = false;
                      if (!text.empty())
                        this->commitText(text);
                      committedLen_ = uinputPendingFinalLen_;
                      uinputPendingFinalLen_ = 0;
                      if (!bufferedUinputKeys_.empty())
                        replayBufferedUinputKeys();
                      return true;
                    });
          } else {
            surroundingCommit(preRestore, postRestore);
          }
        }
      }
      saveLastWord();
      if (useSurroundingText()) {
        forceFlushDeferredCommit();
      } else {
        commitBuffer();
        clearUI();
      }
      viet_.reset();
      if (!autoRestored) {
        committedLen_ = 0;
      }
      if (autoRestored && useUinputMode()) {
        keyEvent.filterAndAccept();
      }
    }
    return;
  }

  if (key.check(FcitxKey_Delete) && inChromiumAddressBar()) {
    addrBarHadFirstWord_ = false;
    addrBarDidFullReplace_ = false;
    addrBarKeepState_ = false;
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
          if (!sepAlreadyDeleted_) {
            ic_->deleteSurroundingText(-1, 1);
            if (ic_->surroundingText().isValid()) {
              ic_->surroundingText().deleteText(-1, 1);
            }
          }
          reclaimLastWord();
          didReclaim = true;
        }
        reclaimReady_ = false;
        sepAlreadyDeleted_ = false;
      }

      // Starting a new word with empty raw input: clear any residual
      // English bypass from a previous undo (e.g. from a re-delivered
      // tone key that triggered tryUndoTransform).  Without this, all
      // subsequent words lose Vietnamese composition.
      if (viet_.getRawInput().empty()) {
        viet_.clearEnglishBypass();
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
          if (isChromiumCached() && !oldComposed.empty()) {
            addrBarLastTriggerKey_ = static_cast<int>(sym);
            addrBarTriggerDeadline_ = now(CLOCK_MONOTONIC) + 100000;
          }
          // Save the finalized word so reclaim can restore the correct
          // word when the user later backspaces through its separator.
          // Must be called before surroundingCommit which updates
          // committedLen_ to the new value.
          if (!committed.empty()) {
            saveLastWord();
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
            // Snapshot committedLen_ at the start of a new word
            // (oldComposed empty).  Used in scheduleAddrBarReplacement
            // to check if the bar was empty before this word.
            if (oldComposed.empty()) {
              addrBarPrevCommittedLen_ = committedLen_;
            }
            committedLen_ = static_cast<int>(utf8::length(newComposed));

            // Check matching append: old + key == new
            if (oldComposed + keyUtf8 == newComposed) {
              // Forward raw X11 key — instant, no D-Bus latency.
              // Set cycle protection + trigger-key guard: subsequent
              // replacement may trigger spurious focus changes in Chromium
              // address bar, causing Chrome to re-deliver the forwarded
              // key.  The guard drops re-delivered keys within 200ms.
              if (inChromiumAddressBar()) {
                addrBarExpectCycle_ = true;
                // Set a short trigger-key guard: Chrome may re-deliver the
                // forwarded key during a spurious focus cycle (~5ms).  A
                // 50ms window catches re-delivery while letting deliberate
                // double-presses (aa→â, dd→đ) through (>100ms typical).
                addrBarLastTriggerKey_ = static_cast<int>(sym);
                addrBarTriggerDeadline_ = now(CLOCK_MONOTONIC) + 50000;
              }
              SKEY_DEBUG() << "Uinput: forward append '" << keyUtf8 << "'";
              // fcitx5 will call reset() after unfiltered key; the
              // guard preserves viet_ for Firefox/Snap apps.
              if (isFirefoxOrSnap()) {
                uinputKeyForwarded_ = true;
              }
              return; // forward raw key
            }

            // Non-matching: consume key, send BS via uinput,
            // replacement text via commitString with adaptive
            // delay to let the app process uinput BS first.
            // Always apply the Vietnamese transform — Unikey-style
            // free typing (users undo unwanted transforms manually).
            size_t pfx = commonUtf8PrefixBytes(oldComposed, newComposed);
            std::string delPart = oldComposed.substr(pfx);
            std::string addPart = newComposed.substr(pfx);
            int deleteLen = static_cast<int>(utf8::length(delPart));

            keyEvent.filterAndAccept();

            if (deleteLen > 0) {
              SKEY_DEBUG() << "Uinput: consume '" << keyUtf8
                           << "' replace (del=" << deleteLen << " add='"
                           << addPart << "')";
              // Chromium address bar on X11: use forwardKey (D-Bus)
              // for BackSpace instead of uinput so both BS and
              // commitString travel the same channel — D-Bus ordering
              // guarantees commitString arrives after BS is processed.
              if (inChromiumAddressBar()) {
                bool oldAscii = true;
                for (unsigned char c : oldComposed) {
                  if (c > 127) {
                    oldAscii = false;
                    break;
                  }
                }
                committedLen_ = static_cast<int>(utf8::length(newComposed));
                scheduleAddrBarReplacement(
                    deleteLen, addPart,
                    static_cast<int>(utf8::length(oldComposed)),
                    static_cast<int>(sym), newComposed, oldAscii);
                return;
              }
              sendBackspaceUinput(deleteLen + 1); // +1 sync BS
              expectedUinputBackspaces_ = deleteLen;
              seenUinputBackspaces_ = 0;
              pendingUinputCommit_ = addPart;
              uinputPendingFinalLen_ =
                  static_cast<int>(utf8::length(newComposed));
              uinputDeleting_ = true;
              // Safety: force-commit if BS events are lost
              armUinputSafetyTimer();
            } else if (!addPart.empty()) {
              SKEY_DEBUG() << "Uinput: consume '" << keyUtf8 << "' commit '"
                           << addPart << "'";
              if (inChromiumAddressBar())
                addrBarExpectCycle_ = true;
              commitText(addPart);
            }
            committedLen_ = static_cast<int>(utf8::length(newComposed));
            return;
          }
          // SurroundingText path in Chromium: set trigger-key guard so
          // X11 re-delivery after forwardKey-induced focus cycles is dropped.
          // Only for replacements (oldComposed non-empty), not first char.
          if (isChromiumCached() && !oldComposed.empty() &&
              oldComposed != newComposed) {
            addrBarLastTriggerKey_ = static_cast<int>(sym);
            addrBarTriggerDeadline_ = now(CLOCK_MONOTONIC) + 100000;
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
      // Auto-restore: when enabled, restore non-Vietnamese words
      // to their raw form at commit time (Unikey-style).
      bool autoRestored = false;
      {
        std::string preRestore = viet_.getComposed();
        viet_.autoRestore();
        std::string postRestore = viet_.getComposed();
        if (preRestore != postRestore && useSurroundingText()) {
          autoRestored = true;
          SKEY_DEBUG() << "AutoRestore: '" << preRestore
                       << "' -> '" << postRestore << "'";
          if (useUinputMode()) {
            // Full replacement via uinput: include punctuation in
            // pending commit so it arrives AFTER BS events.
            int oldLen = static_cast<int>(utf8::length(preRestore));
            sendBackspaceUinput(oldLen + 1); // +1 sync BS
            expectedUinputBackspaces_ = oldLen;
            seenUinputBackspaces_ = 0;
            pendingUinputCommit_ = postRestore + std::string(1, ch);
            uinputPendingFinalLen_ =
                static_cast<int>(utf8::length(postRestore));
            uinputDeleting_ = true;
            committedLen_ = uinputPendingFinalLen_;
            uinputSafetyTimer_ =
                engine_->instance()->eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC,
                    now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec,
                    0, [this](EventSourceTime *, uint64_t) {
                      SKEY_DEBUG()
                          << "AutoRestore: safety timeout, force commit";
                      uinputSafetyTimer_.reset();
                      uinputCommitTimer_.reset();
                      std::string text = std::move(pendingUinputCommit_);
                      pendingUinputCommit_.clear();
                      expectedUinputBackspaces_ = 0;
                      seenUinputBackspaces_ = 0;
                      uinputDeleting_ = false;
                      if (!text.empty())
                        this->commitText(text);
                      committedLen_ = uinputPendingFinalLen_;
                      uinputPendingFinalLen_ = 0;
                      if (!bufferedUinputKeys_.empty())
                        replayBufferedUinputKeys();
                      return true;
                    });
          } else {
            surroundingCommit(preRestore, postRestore);
          }
        }
      }
      saveLastWord();
      if (useSurroundingText()) {
        bool hadDeferred = hasDeferredCommitPending();
        if (hadDeferred) {
          pendingFlushSuffix_ += std::string(1, ch);
        }
        forceFlushDeferredCommit();
        if (!hadDeferred && !(autoRestored && useUinputMode())) {
          ic_->commitString(std::string(1, ch));
        }
        keyEvent.filterAndAccept();
      } else {
        commitBuffer();
        clearUI();
      }
      viet_.reset();
      // When autoRestored, the uinput commit handler sets the final
      // committedLen_ asynchronously — don't race with 0 here.
      if (!autoRestored) {
        committedLen_ = 0;
      }
      // Like space, a separator commits the current word — the next
      // word is not the first and must not trigger fullReplace.
      addrBarHadFirstWord_ = true;
    } else if (reclaimReady_) {
      // User typed space/punctuation after backspacing the separator
      // but before a tone key — they want to start a new word, not
      // edit the previous word's tone.  Cancel reclaim so subsequent
      // tone keys don't resurrect the old word.
      SKEY_DEBUG() << "Reclaim: cancelled by non-letter key";
      reclaimReady_ = false;
      sepAlreadyDeleted_ = false;
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
                                           const std::string &fullComposed,
                                           bool oldComposedIsAscii) {
  // Use uinput BS + adaptive EWMA timer delay before D-Bus commitString.
  // uinput BS goes through kernel → Chrome processes it as a real keystroke
  // (omnibox update, autocomplete dismissal).  The EWMA-based delay adapts
  // to Chrome's actual processing speed — faster machines get lower latency.
  //
  // Platform-specific autocomplete handling:
  // - X11: Chrome does NOT send CapabilityFlag::Url.  SurroundingText may be
  //   unavailable in Uinput mode.  Use first-word FullReplace (oldComposedLen
  //   + 1 BS) to dismiss autocomplete, with autoRestore to fix invalid
  //   Vietnamese words (e.g. "vibẻ"→"viber").
  // - Wayland: Chrome accurately reports Url for the address bar.
  //   SurroundingText is reliable.  Use dynamic isAutofillCertain() to
  //   detect autocomplete selections.
  addrBarExpectCycle_ = true;
  if (bs > 0) {
    int totalBs = bs;
    std::string commitText = text;

    if (!isWayland()) {
      // ── X11: First-word FullReplace + autoRestore ──
      // Only the first word after focus gets FullReplace (oldComposedLen
      // + 1 BS to dismiss Chrome autocomplete).  Subsequent words use
      // plain replacement (exact BS count, no Escape) — the forwarded
      // matching-append keys already race on X11, and adding Escape or
      // extra BS only makes the race condition worse.
      //
      // addrBarPrevCommittedLen_ is a snapshot of committedLen_ before the
      // current replacement.  If < 0 (sentinel -1 from backspacing past
      // all tracked text), the bar is empty — reset first-word flag so
      // FullReplace fires again.  Do NOT reset when == 0 (engine reset
      // after space — text still exists on screen, FullReplace would
      // delete the space and join words).
      if (addrBarPrevCommittedLen_ < 0) {
        addrBarHadFirstWord_ = false;
        addrBarHadSpace_ = false;
      }
      if (!addrBarHadFirstWord_ && oldComposedLen > 0 &&
          !fullComposed.empty()) {
        bool hasTextBefore = false;
        const auto &surrounding = ic_->surroundingText();
        if (surrounding.isValid()) {
          hasTextBefore =
              surrounding.cursor() >
              static_cast<unsigned int>(oldComposedLen);
        } else if (addrBarHadSpace_ && addrBarPrevCommittedLen_ > 0) {
          // Space was typed AND there were tracked chars on screen
          // before this word → text exists before cursor → block
          // FullReplace to avoid damaging it.  If prevCommittedLen
          // <= 0 (bar was empty, all tracked text deleted), the
          // space guard is stale — allow FullReplace.
          hasTextBefore = true;
        }
        if (!hasTextBefore) {
          totalBs = oldComposedLen + 1;
          commitText = fullComposed;
          {
            std::string preRestore = commitText;
            viet_.autoRestore();
            std::string postRestore = viet_.getComposed();
            if (preRestore != postRestore) {
              SKEY_DEBUG() << "AddrBar: autoRestore '" << preRestore
                           << "' -> '" << postRestore << "'";
              commitText = postRestore;
            }
          }
          addrBarHadFirstWord_ = true;
          addrBarDidFullReplace_ =
              !(oldComposedIsAscii && oldComposedLen == 1);
          addrBarKeepState_ =
              (oldComposedIsAscii && oldComposedLen == 1);
          SKEY_DEBUG() << "AddrBar: first word, fullReplace BS=" << totalBs
                       << " commit='" << commitText << "'"
                       << (addrBarKeepState_ ? " [keep-state]" : "");
        }
      }
    } else {
      // ── Wayland: First-word FullReplace + dynamic autocomplete ──
      // SurroundingText is reliable on Wayland — use cursor position
      // to safely determine if FullReplace is safe (no text before).
      // FullReplace commits the entire composed word after autoRestore,
      // fixing invalid Vietnamese words (e.g. "vibẻ"→"viber").
      if (!addrBarHadFirstWord_ && oldComposedLen > 0 &&
          !fullComposed.empty()) {
        bool hasTextBefore = false;
        const auto &surrounding = ic_->surroundingText();
        if (surrounding.isValid()) {
          hasTextBefore =
              surrounding.cursor() >
              static_cast<unsigned int>(oldComposedLen);
        }
        if (!hasTextBefore) {
          totalBs = oldComposedLen + 1;
          commitText = fullComposed;
          {
            std::string preRestore = commitText;
            viet_.autoRestore();
            std::string postRestore = viet_.getComposed();
            if (preRestore != postRestore) {
              SKEY_DEBUG() << "AddrBar: autoRestore '" << preRestore
                           << "' -> '" << postRestore << "'";
              commitText = postRestore;
            }
          }
          addrBarHadFirstWord_ = true;
          SKEY_DEBUG() << "AddrBar: first word, fullReplace BS=" << totalBs
                       << " commit='" << commitText << "'";
        }
      } else if (isAutofillCertain()) {
        ++totalBs;
        SKEY_DEBUG() << "AddrBar: autofill detected, +1 BS (total="
                     << totalBs << ")";
      }
    }

    // Trigger-key guard: prevent Chrome's re-delivered key (from focus
    // cycle after commitString) from being processed as a second key press.
    // Only set when triggerKeySym is non-zero — the SurroundingText path
    // already sets the guard before calling us (with the correct sym) and
    // passes triggerKeySym=0 here; we must not overwrite that with 0.
    if (triggerKeySym != 0) {
      addrBarLastTriggerKey_ = triggerKeySym;
      addrBarTriggerDeadline_ = now(CLOCK_MONOTONIC) +
                                (isWayland() ? 100000 : 200000);
    }
    // Sync BS handler uses this to restore committedLen_ after BS
    // pass-through decrements it (same as general uinput path).
    uinputPendingFinalLen_ = static_cast<int>(utf8::length(commitText));
    // No Escape on either platform — extra BS handles autocomplete
    // dismissal.  Escape causes spurious Chrome focus cycles on X11
    // that corrupt the replacement.
    uint32_t uinputFlags = 0;
    expectedUinputBackspaces_ = totalBs;
    seenUinputBackspaces_ = 0;
    pendingUinputCommit_ = commitText;
    uinputDeleting_ = true;
    sendBackspaceUinput(totalBs + 1, "", uinputFlags); // +1 sync BS
    // Safety: force-commit if BS events are lost
    uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec, 0,
        [this](EventSourceTime *, uint64_t) {
          SKEY_DEBUG() << "Uinput: safety timeout, force commit";
          uinputSafetyTimer_.reset();
          std::string text = std::move(pendingUinputCommit_);
          pendingUinputCommit_.clear();
          expectedUinputBackspaces_ = 0;
          seenUinputBackspaces_ = 0;
          uinputDeleting_ = false;
          if (!text.empty())
            this->commitText(text);
          if (uinputPendingFinalLen_ > 0) {
            committedLen_ = uinputPendingFinalLen_;
            uinputPendingFinalLen_ = 0;
          }
          if (addrBarDidFullReplace_) {
            addrBarDidFullReplace_ = false;
            addrBarKeepState_ = false;
            if (!text.empty()) {
              committedLen_ = static_cast<int>(utf8::length(text));
            }
          }
          if (!bufferedUinputKeys_.empty())
            replayBufferedUinputKeys();
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
      (bsRtEwma_ > 0 && bsRtEwma_ != uinputTiming().bsRtInitialUsec)
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
      (bsRtEwma_ > 0 && bsRtEwma_ != uinputTiming().bsRtInitialUsec)
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
          scheduleAddrBarReplacement(
              deleteLen, addedPart,
              static_cast<int>(utf8::length(oldComposed)),
              0, newComposed);
          return;
        }
        if (useUinputMode()) {
          sendBackspaceUinput(deleteLen + 1); // +1 sync BS
          expectedUinputBackspaces_ = deleteLen;
          seenUinputBackspaces_ = 0;
          pendingUinputCommit_ = addedPart;
          uinputPendingFinalLen_ = newLen;
          uinputDeleting_ = true;
          // Safety: force-commit if BS events are lost
          uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
              CLOCK_MONOTONIC,
              now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec, 0,
              [this](EventSourceTime *, uint64_t) {
                SKEY_DEBUG() << "Uinput: safety timeout, force commit";
                uinputSafetyTimer_.reset();
                uinputCommitTimer_.reset();
                std::string text = std::move(pendingUinputCommit_);
                pendingUinputCommit_.clear();
                expectedUinputBackspaces_ = 0;
                seenUinputBackspaces_ = 0;
                uinputDeleting_ = false;
                if (!text.empty())
                  this->commitText(text);
                committedLen_ = uinputPendingFinalLen_;
                uinputPendingFinalLen_ = 0;
                if (!bufferedUinputKeys_.empty())
                  replayBufferedUinputKeys();
                return true;
              });
          committedLen_ = newLen;
          return;
        }
        // SurroundingText forwardKey path: D-Bus forwardKey may trigger
        // Chrome focus cycles (omnibox autocomplete).  Protect engine state
        // even when inChromiumAddressBar() wasn't detected (AT-SPI2 race).
        if (isChromiumCached()) {
          addrBarExpectCycle_ = true;
        }
        for (int i = 0; i < deleteLen; ++i) {
          ic_->forwardKey(Key(FcitxKey_BackSpace));
        }
        committedLen_ = newLen;
        if (!addedPart.empty()) {
          if (isWayland() && isChromiumCached()) {
            // Wayland has no D-Bus FIFO between forwardKey and
            // commitString — an immediate commit races the forwarded
            // BackSpace keys and eats characters (observed in Chromium).
            // Commit only after the app has had time to process them.
            // Pass the stable prefix (already on screen after the BS
            // deletes): follow-up keystrokes then extend only the pending
            // suffix, otherwise the whole word would be re-committed over
            // the prefix ("chào" → "chchào").
            scheduleDeferredCommit(addedPart, stablePrefix);
          } else {
            // Non-Chromium apps process forwarded BS + commit in order
            // (Telegram etc.) — commit immediately, no extra latency.
            commitText(addedPart);
          }
        }
      };

      // Chromium address bar: use uinput BS + adaptive timer
      // (scheduleAddrBarReplacement) instead of the native surrounding-text
      // API.  Chrome's autocomplete can modify text between delete and
      // commit, causing corruption when deleteSurroundingText races with
      // omnibox updates.  The uinput BS approach lets Chrome process the
      // deletion as real keystrokes before we commit the replacement.
      if (useNativeSurroundingApi() && !inChromiumAddressBar()) {
        const auto &surrounding = ic_->surroundingText();
        bool cacheStale = !surroundingCacheEndsWith(surrounding, oldComposed);
        if (!surrounding.isValid() ||
            surrounding.cursor() < static_cast<unsigned int>(deleteLen) ||
            cacheStale) {
          // SurroundingText capability was advertised but the runtime
          // data is invalid, missing, or stale (does not end with the text
          // we are replacing).  Fall back to forwardKey/Uinput for this
          // replacement — deleting via a stale cache removes the wrong
          // characters and duplicates the committed text.
          if (!surrounding.isValid()) {
            // The app never reports surrounding text (LibreOffice,
            // Telegram...).  Mark as failed so subsequent replacements use
            // Uinput instead of retrying a dead API.
            surroundingTextFailed_ = true;
            modeCacheValid_ = false;
            SKEY_DEBUG()
                << "Surr: surrounding text invalid, downgrading to uinput";
          } else if (cacheStale) {
            SKEY_DEBUG() << "Surr: surrounding cache stale, falling back";
          } else {
            SKEY_DEBUG() << "Surr: native surrounding not ready";
          }
          deleteViaBackspace();
        } else {
          // Delete one character at a time.  Chrome has been observed to
          // drop multi-char delete_surrounding_text requests (the commit
          // then duplicates text), while single-char deletes share the
          // commitString protocol channel and are reliable.
          for (int i = 0; i < deleteLen; ++i) {
            ic_->deleteSurroundingText(-1, 1);
            if (ic_->surroundingText().isValid()) {
              ic_->surroundingText().deleteText(-1, 1);
            }
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
    const auto &surrounding = ic_->surroundingText();
    if (!surrounding.isValid()) {
      // Surrounding text advertised but not actually available.
      // Mark as failed and fall back to forwardKey.
      surroundingTextFailed_ = true;
      modeCacheValid_ = false;
      SKEY_DEBUG() << "SurrBS: surrounding text invalid, downgrading to uinput";
      ic_->forwardKey(Key(FcitxKey_BackSpace));
    } else {
      ic_->deleteSurroundingText(-1, 1);
      if (ic_->surroundingText().isValid()) {
        ic_->surroundingText().deleteText(-1, 1);
      }
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

void SKeyState::loadUserDict() {
  // User dictionary — one word per line, '#' starts a comment.
  // Words are added to the engine and checked before the embedded list,
  // so users can keep words the built-in dictionary lacks.
  viet_.clearWords();
  std::string userDir =
      fcitx::StandardPaths::global()
          .userDirectory(fcitx::StandardPathsType::PkgData)
          .string();
  std::string path = userDir + "/skey/user-dict.txt";
  std::ifstream in(path);
  if (!in.is_open()) return;
  std::string line;
  int count = 0;
  while (std::getline(in, line)) {
    size_t b = line.find_first_not_of(" \t\r\n");
    if (b == std::string::npos || line[b] == '#') continue;
    size_t e = line.find_last_not_of(" \t\r\n");
    std::string word = line.substr(b, e - b + 1);
    if (word.empty()) continue;
    viet_.addWord(word);
    ++count;
  }
  if (count > 0) {
    SKEY_INFO() << "User dict: loaded " << count << " word(s) from " << path;
  }
}

void SKeyState::armUinputSafetyTimer() {
  auto &timing = uinputTiming();
  uint64_t budget =
      uinputSafetyRetried_ ? timing.safetyRetryUsec : timing.safetyTimeoutUsec;
  uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
      CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + budget, 0,
      [this](EventSourceTime *, uint64_t) {
        // If our sent BS are still outstanding, they may just be slow
        // (loopback latency > safety window).  Extend the window once —
        // force-committing now would corrupt the screen when the late
        // BS deletions arrive afterwards.
        if (uinputBsOutstanding_ > 0 && !uinputSafetyRetried_) {
          uinputSafetyRetried_ = true;
          SKEY_DEBUG() << "Uinput: BS loopbacks slow — extend safety window";
          armUinputSafetyTimer();
          return true;
        }
        SKEY_DEBUG() << "Uinput: safety timeout, force commit";
        uinputSafetyRetried_ = false;
        uinputSafetyTimer_.reset();
        std::string text = std::move(pendingUinputCommit_);
        pendingUinputCommit_.clear();
        expectedUinputBackspaces_ = 0;
        seenUinputBackspaces_ = 0;
        uinputBsOutstanding_ = 0;
        uinputDeleting_ = false;
        if (!text.empty())
          this->commitText(text);
        if (uinputPendingFinalLen_ > 0) {
          committedLen_ = uinputPendingFinalLen_;
          uinputPendingFinalLen_ = 0;
        }
        if (!bufferedUinputKeys_.empty())
          replayBufferedUinputKeys();
        return true;
      });
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
  sepAlreadyDeleted_ = false;
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

  // Build candidate list for dropdown menu
  auto candList = std::make_unique<CommonCandidateList>();
  candList->setPageSize(5);
  candList->setLayoutHint(CandidateLayoutHint::Vertical);

  int cursorIdx = 0;
  if (modeMenuForAddressBar_) {
    auto addressBarMode = engine_->config().chromiumAddressBarMode.value();
    std::string autoLabel = "1. Auto (";
    autoLabel += outputModeName(effectiveMode());
    autoLabel += ")";
    candList->append(std::make_unique<AddressBarModeCandidateWord>(
        engine_, this, autoLabel, SKeyChromiumAddressBarMode::Auto));
    candList->append(std::make_unique<AddressBarModeCandidateWord>(
        engine_, this, "2. Uinput", SKeyChromiumAddressBarMode::Uinput));
    candList->append(std::make_unique<AddressBarModeCandidateWord>(
        engine_, this, "3. Surrounding Text",
        SKeyChromiumAddressBarMode::SurroundingText));
    candList->append(std::make_unique<AddressBarModeCandidateWord>(
        engine_, this, "4. Preedit", SKeyChromiumAddressBarMode::Preedit));
    candList->append(std::make_unique<AddressBarModeCandidateWord>(
        engine_, this, "5. Không gõ tiếng Việt",
        SKeyChromiumAddressBarMode::NoVietnamese));
    cursorIdx = static_cast<int>(addressBarMode);
  } else {
    auto configured = hasAppModeOverride_
                          ? appModeOverride_
                          : engine_->config().outputMode.value();
    std::string autoLabel = "1. Auto (";
    autoLabel += outputModeName(effectiveMode());
    autoLabel += ")";
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, autoLabel, SKeyOutputMode::Auto));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "2. Uinput", SKeyOutputMode::Uinput));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "3. Surrounding Text", SKeyOutputMode::SurroundingText));
    candList->append(std::make_unique<ModeCandidateWord>(
        engine_, this, "4. Preedit", SKeyOutputMode::Preedit));

    std::string excludeLabel =
        appExcluded_ ? "5. ✓ Loại trừ ứng dụng" : "5. Loại trừ ứng dụng";
    candList->append(
        std::make_unique<ExcludeCandidateWord>(engine_, this, excludeLabel));

    // When configured to Auto, cursor stays on Auto (0).
    // Otherwise cursor follows the manually selected mode.
    cursorIdx = appExcluded_                                      ? 4
                : (configured == SKeyOutputMode::Auto)            ? 0
                : (configured == SKeyOutputMode::Uinput)          ? 1
                : (configured == SKeyOutputMode::SurroundingText) ? 2
                : (configured == SKeyOutputMode::Preedit)         ? 3
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
