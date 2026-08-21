#include "engine.h"

#include "charset.h"
#include "icon_resolver.h"
#include "x11_app_name.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpath.h>
// StandardPaths (fcitx5 >= 5.1) replaces the deprecated StandardPath API.
// Older distros (Ubuntu 22.04 CI, fcitx5 5.0.x) only ship standardpath.h —
// detect at compile time and fall back (see userPkgDataDir()).
#ifndef SKEY_HAVE_STANDARDPATHS
#if defined(__has_include)
#if __has_include(<fcitx-utils/standardpaths.h>)
#define SKEY_HAVE_STANDARDPATHS 1
#endif
#endif
#endif
#ifdef SKEY_HAVE_STANDARDPATHS
#include <fcitx-utils/standardpaths.h>
#endif

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
  double chromiumDelayFactor; // extra multiplier for Chromium/Electron apps
  uint64_t safetyTimeoutUsec; // force-commit if BS events don't arrive
  uint64_t safetyRetryUsec;   // extended window when BS are just slow
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
};

// Wayland: clamp delay to [2ms, 12ms] for native apps.
// Electron/Chromium apps get a 1.5× boost via chromiumDelayFactor.
static constexpr UinputTiming kUinputTimingWayland = {
    0.3,    // bsRtEwmaAlpha — moderate adaptation
    3000,   // bsRtInitialUsec
    0.5,    // bsRtMultiplier
    0.5,    // addrBarBsRtMultiplier
    2000,   // commitDelayMinUsec — 2ms floor (prevents char loss on Qt/GTK)
    2000,   // addrBarCommitDelayMinUsec
    12000,  // commitDelayMaxUsec — 12ms cap
    12000,  // addrBarCommitDelayMaxUsec
    1.5,    // chromiumDelayFactor — Electron multi-process needs 1.5×
    80000,  // safetyTimeoutUsec (80ms)
    600000, // safetyRetryUsec (600ms) — one extension for slow loopbacks
};

// Surr deferred commit timing (same mechanism, independent of uinput path)
static constexpr uint64_t dbusDeferredDefaultUsec = 15000;
static constexpr uint64_t dbusDeferredMinUsec = 10000;

// Uinput commit-delay for non-Chromium apps on native Wayland.  The anchor
// loopback proves fcitx5 saw the BS, not that the app processed them — Qt
// apps queue injected keys and lag behind the commit ("tại" → "taạ").  The
// delay scales with the number of deletions: each injected BS needs ~8ms of
// queue-drain headroom before the commit lands (a fixed 15ms floor made
// single-deletion replacements visibly flicker).
static constexpr uint64_t kWaylandNativeCommitDelayPerBsUsec = 6000;
static constexpr uint64_t kWaylandNativeCommitDelayMaxUsec = 30000;
// Floor applied on the next commit after loopbacks were slow once.
static constexpr uint64_t kSlowLoopbackCommitDelayFloorUsec = 15000;
// Standalone Chromium/Electron apps (antigravity): "Uinput (Slow)"
// treatment — a generous fixed sleep plus a bounded surrounding-cursor
// verification before the commit.  Surrounding pushes are throttled by
// Chromium (~50ms+), too slow to drive a confirm-wait, so the sleep is
// fixed and the verification only buys a few extra milliseconds.
static constexpr uint64_t kUinputSlowModeSleepUsec = 20000;
static constexpr int kUinputSlowModeVerifyRetries = 3;
static constexpr uint64_t kUinputSlowModeRetryIntervalUsec = 2000;

// NOTE: AT-SPI2 queries for the Chromium address bar must NEVER run on
// the fcitx5 main thread — a stuck DBus reply blocks all input handling
// (observed 0.9–7.5s stalls).  The A11yMonitor thread polls the omnibox
// text + selection in the background and the engine reads its atomic
// snapshot instead.
static constexpr uint64_t kA11ySnapshotMaxAgeUsec = 400000;

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

// Probe helper: one-shot connect to the uinput server (fs socket, then
// legacy abstract name), returning a trusted fd or -1.  Defined near
// connectUinputServer (needs peerIsTrusted).
static int probeUinputServer();

struct UinputSocketPaths {
  std::string fsPath;       // "/run/skey-uinput-<user>/kb_socket"; empty if
                            // longer than sun_path
  std::string abstractName; // legacy "skeysocket-<user>-kb_socket"
};

static UinputSocketPaths uinputSocketPaths(const char *suffix) {
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

  UinputSocketPaths paths;
  paths.abstractName = std::string("skeysocket-") + username + "-" + suffix;
  constexpr size_t maxAbstractSocketName =
      sizeof(((sockaddr_un *)0)->sun_path) - 1;
  if (paths.abstractName.size() > maxAbstractSocketName) {
    paths.abstractName.resize(maxAbstractSocketName);
  }
  paths.fsPath = "/run/skey-uinput-" + username + "/" + suffix;
  if (paths.fsPath.size() >= sizeof(((sockaddr_un *)0)->sun_path)) {
    paths.fsPath.clear();
  }
  return paths;
}

FCITX_DEFINE_LOG_CATEGORY(skey_log, "skey");
#define SKEY_DEBUG() SKeyLogger()
#define SKEY_INFO() SKeyLogger()

/// Path to fcitx5's per-user package-data directory
/// (~/.local/share/fcitx5).  fcitx5 >= 5.1 provides StandardPaths;
/// older distros only have the legacy StandardPath API.
static std::string userPkgDataDir() {
#ifdef SKEY_HAVE_STANDARDPATHS
  return fcitx::StandardPaths::global()
      .userDirectory(fcitx::StandardPathsType::PkgData)
      .string();
#else
  return fcitx::StandardPath::global().userDirectory(
      fcitx::StandardPath::Type::PkgData);
#endif
}

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

static bool isTerminalAppName(const std::string &prog) {
  // Terminals have their own internal buffer — SurroundingText API
  // doesn't sync correctly, so Uinput raw key pass-through works better.
  // Electron terminals (Tabby, Hyper) advertise the SurroundingText
  // capability but apply delete_surrounding_text unreliably (deletes
  // dropped or reordered against commitString), corrupting every toned
  // word — route them to Uinput like native terminals.
  //
  // This static list is only the fast path; unknown terminals are
  // auto-detected by the shell-descendant scan (processHasShellChild).
  static const char *const patterns[] = {
      "konsole",        "org.kde.konsole",
      "alacritty",      "kitty",
      "gnome-terminal", "xfce4-terminal",
      "sterm",          "st-",
      "terminator",     "terminology",
      "wezterm",        "foot",
      "urxvt",          "rxvt",
      "xterm",          "tabby",
      "hyper",
  };
  for (const char *p : patterns) {
    if (prog.find(p) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Check a KNOWN pid for Chromium markers: the exe path (or any ppid
// ancestor's exe) contains electron/chrome/chromium, or the exe's
// directory ships chrome-sandbox / chrome_crashpad_handler (renamed
// Electron shells like antigravity).  Bounded to 10 generations.
static bool processHasChromiumMarkers(pid_t pid) {
  int cur = static_cast<int>(pid);
  for (int depth = 0; depth < 10; ++depth) {
    std::string checkPid = std::to_string(cur);
    char exeBuf[PATH_MAX];
    std::string exePath = "/proc/" + checkPid + "/exe";
    ssize_t len = readlink(exePath.c_str(), exeBuf, sizeof(exeBuf) - 1);
    if (len > 0) {
      exeBuf[len] = '\0';
      std::string lower(exeBuf);
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      if (lower.find("electron") != std::string::npos ||
          lower.find("chrome") != std::string::npos ||
          lower.find("chromium") != std::string::npos) {
        return true;
      }
      size_t lastSlash = lower.rfind('/');
      if (lastSlash != std::string::npos) {
        std::string exeDir = lower.substr(0, lastSlash);
        if (access((exeDir + "/chrome-sandbox").c_str(), F_OK) == 0 ||
            access((exeDir + "/chrome_crashpad_handler").c_str(), F_OK) ==
                0) {
          return true;
        }
      }
    }
    std::string statPath = "/proc/" + checkPid + "/stat";
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
    int ppid = 0;
    iss >> state >> ppid;
    if (ppid <= 1 || ppid == cur)
      break;
    cur = ppid;
  }
  return false;
}

// Check a KNOWN pid for a shell in its descendant chain (terminals spawn
// bash/zsh/fish/... as children).  BFS over /proc/<pid>/task/<pid>/children
// with a depth bound — no full /proc scan.
static bool processHasShellChildPid(pid_t pid) {
  static const char *const shells[] = {"bash", "zsh", "fish", "sh",
                                       "dash", "ksh",  "tcsh", "csh"};
  std::vector<pid_t> frontier;
  frontier.push_back(pid);
  for (int depth = 0; depth < 4 && !frontier.empty(); ++depth) {
    std::vector<pid_t> next;
    for (pid_t p : frontier) {
      std::string childrenPath = "/proc/" + std::to_string(p) + "/task/" +
                                 std::to_string(p) + "/children";
      std::ifstream childrenFile(childrenPath);
      std::string line;
      if (!childrenFile.is_open() || !std::getline(childrenFile, line))
        continue;
      std::istringstream iss(line);
      pid_t child;
      while (iss >> child) {
        std::string commPath = "/proc/" + std::to_string(child) + "/comm";
        std::ifstream commFile(commPath);
        std::string comm;
        if (commFile.is_open() && std::getline(commFile, comm)) {
          for (const char *s : shells) {
            if (comm == s)
              return true;
          }
        }
        next.push_back(child);
      }
    }
    frontier = std::move(next);
  }
  return false;
}

// Check a KNOWN pid for a Snap-packaged binary.
static bool processIsSnapPid(pid_t pid) {
  char exeBuf[PATH_MAX];
  std::string exePath = "/proc/" + std::to_string(pid) + "/exe";
  ssize_t len = readlink(exePath.c_str(), exeBuf, sizeof(exeBuf) - 1);
  if (len > 0) {
    exeBuf[len] = '\0';
    if (std::string(exeBuf).find("/snap/") != std::string::npos)
      return true;
  }
  std::ifstream mapsFile("/proc/" + std::to_string(pid) + "/maps");
  std::string line;
  if (mapsFile.is_open() && std::getline(mapsFile, line) &&
      line.find("/snap/") != std::string::npos) {
    return true;
  }
  return false;
}

// Auto-detect terminals by their hosted shell: a terminal emulator
// spawns a shell (bash/zsh/fish/...) as a descendant process, which
// ordinary GUI apps never do.  Scans /proc for a process whose comm
// matches `prog` and that has a shell somewhere in its descendant chain.
static bool processHasShellChild(const std::string &prog) {
  if (prog.empty()) {
    return false;
  }
  static const char *const shells[] = {"bash", "zsh", "fish", "sh",
                                       "dash", "ksh",  "tcsh", "csh"};
  DIR *dir = opendir("/proc");
  if (!dir) {
    return false;
  }
  bool found = false;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_type != DT_DIR || !isdigit(entry->d_name[0]))
      continue;
    pid_t child = static_cast<pid_t>(atoi(entry->d_name));
    // Is this process a descendant of a process whose comm matches prog?
    // Follow ppid links up to 16 generations via /proc/<pid>/stat.
    pid_t cur = child;
    bool isDescendant = false;
    for (int depth = 0; depth < 16 && cur > 1; ++depth) {
      std::string statPath = "/proc/" + std::to_string(cur) + "/stat";
      std::ifstream statFile(statPath);
      if (!statFile.is_open())
        break;
      std::string statLine;
      std::getline(statFile, statLine);
      // comm may contain spaces/parens — the ppid is the field right
      // after the closing paren.
      size_t rp = statLine.rfind(')');
      if (rp == std::string::npos)
        break;
      std::istringstream rest(statLine.substr(rp + 2));
      char state = 0;
      rest >> state;
      pid_t ppid = 0;
      rest >> ppid;
      if (ppid <= 0)
        break;
      // Compare the ancestor's comm with prog (15-char truncation aware).
      std::string commPath = "/proc/" + std::to_string(ppid) + "/comm";
      std::ifstream commFile(commPath);
      std::string comm;
      if (commFile.is_open() && std::getline(commFile, comm) &&
          (comm == prog || prog.compare(0, 15, comm) == 0 ||
           comm.compare(0, 15, prog) == 0)) {
        isDescendant = true;
        break;
      }
      cur = ppid;
    }
    if (!isDescendant)
      continue;
    // Is the descendant itself a shell?
    std::string commPath = "/proc/" + std::to_string(child) + "/comm";
    std::ifstream commFile(commPath);
    std::string comm;
    if (commFile.is_open() && std::getline(commFile, comm)) {
      for (const char *s : shells) {
        if (comm == s) {
          found = true;
          break;
        }
      }
    }
    if (found)
      break;
  }
  closedir(dir);
  return found;
}

/// Detect lock screen / login screen programs by name.
/// These should NEVER process Vietnamese transforms — the user is
/// typing a password.  CapabilityFlag::PasswordOrSensitive (Wayland)
/// and AT-SPI2 (X11) catch most cases, but some lock screens (like
/// KDE's kscreenlocker_greet on X11) don't expose either signal.
static bool programIsLockScreen(const std::string &prog) {
  static const char *const patterns[] = {
      "kscreenlocker", // KDE lock screen (kscreenlocker_greet)
      "i3lock",        // i3 lock screen
      "swaylock",      // Sway lock screen
      "gtklock",       // GTK-based lock screen
      "hyprlock",      // Hyprland lock screen
      "sddm",          // SDDM login manager
      "gdm",           // GDM login manager
      "lightdm",       // LightDM login manager
      "lxdm",          // LXDM login manager
      "polkit",        // polkit auth dialogs
  };
  for (const char *p : patterns) {
    if (prog.find(p) != std::string::npos) {
      return true;
    }
  }
  // Exact match for system auth programs (substring would be too broad)
  if (prog == "login" || prog == "su" || prog == "sudo" || prog == "pkexec") {
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
    engine_->saveAppMode(state_->appProgram(), mode_);
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
    engine_->saveAppExcluded(state_->appProgram(), newExcluded);
    SKEY_INFO() << "App '" << state_->appProgram()
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
  SKEY_INFO() << "Saved app mode: " << app << " -> " << val << " ok=" << ok;
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
    if (!de)
      de = std::getenv("DESKTOP_SESSION");
    return de &&
           (std::string(de) == "cinnamon" || std::string(de) == "X-Cinnamon");
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
  paths.userDataDir = userPkgDataDir();
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

const std::string &SKeyState::appProgram() const {
  const std::string &prog = ic_->program();
  if (!prog.empty()) {
    return prog;
  }
  // The IBus frontend reports an empty program name for apps that only
  // speak the ibus protocol (AppImages etc.) — all such apps would share
  // one "(IBus app)" config key, so per-app mode settings leak between
  // them.  Resolve the real name from the focused X11 window's WM_CLASS,
  // attempted once per focus (see activate()).
  if (appNameAttempted_) {
    return resolvedProgram_;
  }
  appNameAttempted_ = true;
  resolvedProgram_ = x11FocusedWmClass();
  if (!resolvedProgram_.empty()) {
    SKEY_INFO() << "App: resolved program name via X11: '" << resolvedProgram_
                << "'";
  }
  return resolvedProgram_;
}

void SKeyState::refreshAppMode() {
  std::string prog = appProgram();
  if (prog == cachedProgram_)
    return;
  cachedProgram_ = prog;

  hasAppModeOverride_ = false;
  appExcluded_ = false;

  // Per-app config is keyed by the resolved app name — appProgram()
  // falls back to X11 WM_CLASS when the IBus frontend reports an empty
  // program name (AppImages etc.).
  RawConfig cfg;
  readAsIni(cfg, "conf/skey-app-modes.conf");
  auto *val = cfg.valueByPath(prog);
  if (val) {
    std::string modeStr = *val;
    if (modeStr.size() >= 2 && modeStr.front() == '"' && modeStr.back() == '"')
      modeStr = modeStr.substr(1, modeStr.size() - 2);

    if (modeStr == "Excluded") {
      appExcluded_ = true;
    } else {
      SKeyOutputMode savedMode = engine_->config().outputMode.value();
      if (modeStr == "Preedit")
        savedMode = SKeyOutputMode::Preedit;
      else if (modeStr == "SurroundingTextSlow" ||
               modeStr == "SurroundingText" || modeStr == "Surrounding Text")
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
  if (!isWayland() && isChromiumBrowser(appProgram())) {
    auto *mon = engine_->a11yMonitor();
    if (mon && mon->isBrowserUIFocused()) {
      // Fresh true verdict — remember when so a lagging monitor can't
      // flip the decision mid-word.
      addrBarUiVerdictAtUsec_ = now(CLOCK_MONOTONIC);
      return true;
    }
    // Grace window: the monitor processes focus events asynchronously
    // and Chrome's omnibox churn (dropdown open/close, suggestion
    // previews) can flip browserUIFocused_ false between keystrokes —
    // observed as "aâ" corruption on X11 when the retype's tone key
    // fell outside a short window.  5s covers churn and typing pauses
    // while still latching off after a real focus change.
    if (addrBarUiVerdictAtUsec_ != 0 &&
        now(CLOCK_MONOTONIC) - addrBarUiVerdictAtUsec_ <= 5000000) {
      return true;
    }
    addrBarUiVerdictAtUsec_ = 0;
    // Deterministic fallback (no a11y needed): the omnibox cursor rect
    // is a thin 1×~20 sliver near the window top, while Chrome content
    // editors report wider rects or (0,0,0x0).  This keeps the address
    // bar routing alive right after fcitx5 restarts, before the a11y
    // monitor has processed any focus event.
    const auto &rect = ic_->cursorRect();
    if (rect.width() <= 2 && rect.height() >= 18 && rect.height() <= 24 &&
        rect.top() >= 0 && rect.top() < 200) {
      return true;
    }
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
    (1ULL << 3) |  // Password
    (1ULL << 7) |  // Email
    (1ULL << 8) |  // Digit
    (1ULL << 9) |  // Uppercase
    (1ULL << 10) | // Lowercase
    (1ULL << 14) | // Number
    (1ULL << 16) | // SpellCheck
    (1ULL << 17) | // NoSpellCheck
    (1ULL << 18) | // WordCompletion
    (1ULL << 20) | // UppercaseSentences
    (1ULL << 21) | // Alpha
    (1ULL << 22);  // Name

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

bool SKeyState::a11yBrowserNonEntry() const {
  if (!isChromiumCached() || !isChromiumBrowser(appProgram()))
    return false;
  auto *mon = engine_->a11yMonitor();
  return mon && mon->isFocusSnapshotFresh(5000000) &&
         !mon->isTextEntryFocused();
}

bool SKeyState::noteSurroundingFailure() {
  if (isChromiumCached())
    return true;
  ++surroundingInvalidCount_;
  return surroundingInvalidCount_ >= 2;
}

void SKeyState::clearEngineBareCapsSticky() const {
  if (!engine_->chromiumHadBareCaps_) {
    return;
  }
  engine_->chromiumHadBareCaps_ = false;
  engine_->chromiumBareCapsProgram_.clear();
  SKEY_DEBUG() << "Auto: engine sticky bare caps cleared "
                  "(real web editor detected)";
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

  // A Chromium browser tab whose a11y focus is NOT a text entry cannot
  // receive surrounding-text replacements.  Clicking a Google Sheets cell
  // focuses the document/combo box while caps still carry the previous
  // editor's strong hints (e.g. 0x90072 from a chat box) — typing would go
  // nowhere.  Requires a FRESH snapshot: without one we cannot conclude
  // anything and fall back to the cap-based decision below.
  if (a11yBrowserNonEntry()) {
    SKEY_DEBUG() << "Auto: focus is not a text entry → Uinput";
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
    clearEngineBareCapsSticky();
  }

  auto caps = ic_->capabilityFlags();

  if (!caps.test(CapabilityFlag::SurroundingText)) {
    // Native Wayland apps (Telegram) often omit the SurroundingText cap
    // on the compositor text-input path even though they push surrounding
    // text — probe the native API anyway; the runtime validation
    // (surroundingTextFailed_ + retry) downgrades to Uinput when the
    // cache never arrives.
    if (waylandNativeSurroundingProbe()) {
      SKEY_DEBUG() << "Auto: no SurroundingText cap, probing SurroundingText";
      return SKeyOutputMode::SurroundingText;
    }
    SKEY_DEBUG() << "Auto: no SurroundingText cap → Uinput";
    return SKeyOutputMode::Uinput;
  }

  // Terminal apps (Konsole, Alacritty, etc.) have their own internal
  // buffer — SurroundingText API doesn't sync correctly, so Uinput
  // raw key pass-through works better.
  // Shell-scan hits only count for non-Chromium apps: Chromium-family
  // apps with shell children are IDEs with embedded terminals (VS Code,
  // antigravity), not terminals — known Electron terminals (Tabby, Hyper)
  // are already on the name list.
  if (caps.test(CapabilityFlag::Terminal) ||
      (isTerminalAppCached() &&
       (!isChromiumCached() || isTerminalAppName(appProgram())))) {
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
        (1ULL << 3) |  // Password
        (1ULL << 7) |  // Email
        (1ULL << 8) |  // Digit
        (1ULL << 9) |  // Uppercase
        (1ULL << 10) | // Lowercase
        (1ULL << 11) | // NoAutoUpperCase
        (1ULL << 13) | // Dialable
        (1ULL << 14) | // Number
        (1ULL << 15) | // NoOnScreenKeyboard
        (1ULL << 16) | // SpellCheck
        (1ULL << 17) | // NoSpellCheck
        (1ULL << 18) | // WordCompletion
        (1ULL << 19) | // UppercaseWords
        (1ULL << 20) | // UppercaseSentences
        (1ULL << 21) | // Alpha
        (1ULL << 22);  // Name
    CapabilityFlags contentHints(kContentHints);
    if (!(caps & contentHints)) {
      // Standalone Chromium/Electron apps (not browsers) deliver
      // surrounding text via shared Chromium text-input code even with
      // bare caps (verified: antigravity on Wayland — browsers get the
      // bare-caps logic below instead, where Sheets-style tabs need
      // Uinput).  Runtime validation in surroundingCommit() still
      // downgrades via surroundingTextFailed_ if the app is broken.
      // Electron terminals are routed to Uinput earlier via
      // isTerminalApp() (delete_surrounding_text is unreliable there).
      if (!isChromiumBrowser(appProgram())) {
        SKEY_DEBUG() << "Auto: bare caps + standalone Chromium app → "
                        "SurroundingText";
        return SKeyOutputMode::SurroundingText;
      }
      // Fresh AT-SPI2 web-editor focus (Facebook chat) wins over bare caps:
      // Chrome fires the a11y focus event immediately on click, but its
      // content-type caps may stay bare until a text-input re-sync.
      if (a11yFreshWebEditor()) {
        modeDecisionPending_ = false;
        clearEngineBareCapsSticky();
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
        engine_->chromiumBareCapsProgram_ = appProgram();
        engine_->chromiumHadBareCaps_ = true;
        SKEY_DEBUG()
            << "Auto: bare caps confirmed after deferral → sticky Uinput";
      } else if (!modeDecisionPending_) {
        modeDecisionPending_ = true;
        modeDecisionDeadlineUsec_ =
            now(CLOCK_MONOTONIC) + kBareCapsDecisionWindowUsec;
        SKEY_DEBUG() << "Auto: bare caps → Uinput (deferred, caps=0x"
                     << std::hex << static_cast<uint64_t>(caps.toInteger())
                     << std::dec << ")";
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
      clearEngineBareCapsSticky();
      SKEY_DEBUG() << "Auto: deferred decision → SurroundingText (caps=0x"
                   << std::hex << static_cast<uint64_t>(caps.toInteger())
                   << std::dec << ")";
    } else {
      chromiumBareCapsUinput_ = true;
      engine_->chromiumBareCapsProgram_ = appProgram();
      engine_->chromiumHadBareCaps_ = true;
      SKEY_DEBUG()
          << "Auto: deferred decision → sticky Uinput (weak hints only)";
      return SKeyOutputMode::Uinput;
    }
  }

  SKEY_DEBUG() << "Auto: SurroundingText cap → SurroundingText";
  return SKeyOutputMode::SurroundingText;
}

int SKeyState::a11yAppPid() const {
  auto *mon = engine_->a11yMonitor();
  if (!mon || !mon->isFocusSnapshotFresh(5000000)) {
    return -1;
  }
  int pid = mon->focusProcessId();
  if (pid <= 1) {
    return -1;
  }
  const std::string &prog = appProgram();
  if (prog.empty()) {
    return -1;
  }
  std::ifstream commFile("/proc/" + std::to_string(pid) + "/comm");
  std::string comm;
  if (!commFile.is_open() || !std::getline(commFile, comm)) {
    return -1;
  }
  if (!comm.empty() && comm.back() == '\n') {
    comm.pop_back();
  }
  if (comm != prog && prog.compare(0, 15, comm) != 0 &&
      comm.compare(0, 15, prog) != 0) {
    return -1; // the focused accessible belongs to a different app
  }
  return pid;
}

bool SKeyState::isChromiumCached() const {
  if (cachedIsChromium_ < 0) {
    const std::string &prog = appProgram();
    bool chromium = isChromiumBrowser(prog) ||
                    prog.find("electron") != std::string::npos;
    if (!chromium) {
      int pid = a11yAppPid();
      chromium = pid > 0 ? processHasChromiumMarkers(pid)
                         : isChromiumBasedApp(prog);
    }
    cachedIsChromium_ = chromium ? 1 : 0;
  }
  return cachedIsChromium_ == 1;
}

bool SKeyState::isTerminalAppCached() const {
  if (cachedIsTerminalApp_ >= 0) {
    return cachedIsTerminalApp_ == 1;
  }
  const std::string &prog = appProgram();
  // Program-level cache: the verdict is a property of the program, so a
  // /proc scan happens at most once per program per session — subsequent
  // focuses of the same app are O(1).
  if (prog == engine_->terminalCachedProgram_ &&
      engine_->terminalCachedVerdict_ >= 0) {
    cachedIsTerminalApp_ = engine_->terminalCachedVerdict_;
    return cachedIsTerminalApp_ == 1;
  }
  bool term = isTerminalAppName(prog);
  if (!term) {
    int pid = a11yAppPid();
    term = pid > 0 ? processHasShellChildPid(pid) : processHasShellChild(prog);
  }
  cachedIsTerminalApp_ = term ? 1 : 0;
  engine_->terminalCachedProgram_ = prog;
  engine_->terminalCachedVerdict_ = cachedIsTerminalApp_;
  return term;
}

bool SKeyState::isFirefoxOrSnap() const {
  if (cachedIsFirefoxOrSnap_ >= 0) {
    return cachedIsFirefoxOrSnap_ == 1;
  }
  const std::string &prog = appProgram();
  // Firefox program name (native or Snap)
  if (prog.find("firefox") != std::string::npos) {
    cachedIsFirefoxOrSnap_ = 1;
    return true;
  }
  // Detect Snap-packaged apps: prefer the a11y-reported PID (one readlink,
  // no scan) and fall back to the full /proc scan.
  if (!prog.empty()) {
    int pid = a11yAppPid();
    if (pid > 0) {
      cachedIsFirefoxOrSnap_ = processIsSnapPid(pid) ? 1 : 0;
      return cachedIsFirefoxOrSnap_ == 1;
    }
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
          std::string exePath = "/proc/" + std::string(entry->d_name) + "/exe";
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

bool SKeyState::waylandNativeSurroundingProbe() const {
  return isWayland() && !isChromiumCached() && !isTerminalAppCached();
}

bool SKeyState::useNativeSurroundingApi() const {
  // Single effectiveMode() call — avoids double-evaluating detectAutoMode()
  // (which scans /proc via isChromiumBasedApp) on every keystroke.
  auto mode = effectiveMode();
  return mode == SKeyOutputMode::SurroundingText &&
         (ic_->capabilityFlags().test(CapabilityFlag::SurroundingText) ||
          waylandNativeSurroundingProbe());
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

  // Fresh focus: re-resolve the app name for empty-program (IBus frontend)
  // apps — the focused X11 window may have changed.
  appNameAttempted_ = false;
  resolvedProgram_.clear();

  // Reactivate after spurious cycle: cancel the genuine-loss timer.
  if (addrBarExpectCycle_) {
    SKEY_DEBUG() << "Activate: spurious cycle, cancel loss timer";
    addrBarCycleTimer_.reset();
  } else {
    // Spurious-cycle detection: if preeditWasPending_ is set and the
    // activating program matches, the same IC is being reactivated —
    // the app auto-committed on focus loss (e.g., LibreOffice).
    // Don't double-commit; just clean up the engine entry.
    auto it = engine_->pendingPreedits_.find(appProgram());
    if (preeditWasPending_ && preeditPendingProgram_ == appProgram()) {
      SKEY_DEBUG() << "Activate: spurious cycle for '" << appProgram()
                   << "', discarding saved preedit";
      if (it != engine_->pendingPreedits_.end()) {
        engine_->pendingPreedits_.erase(it);
      }
    } else if (it != engine_->pendingPreedits_.end()) {
      // Genuine return — the IC was destroyed and recreated, or the
      // activating program differs from the one that saved the text.
      SKEY_DEBUG() << "Activate: committing saved preedit '" << it->second
                   << "' for program '" << appProgram() << "'";
      commitText(it->second);
      engine_->pendingPreedits_.erase(it);
      ic_->updatePreedit();
    }
    preeditWasPending_ = false;

    // Detect spurious focus cycles that arrived when addrBarExpectCycle_
    // was not armed (e.g. asynchronous omnibox updates).  If reactivation
    // happens within 500ms of deactivate in the same Chromium address bar,
    // preserve the composition AND first-word/space tracking — resetting
    // here would silently commit the word mid-typing and break tone
    // editing after a slow pause, or give the next replacement fullReplace
    // treatment that deletes text before the cursor.
    bool spuriousCycle = inChromiumAddressBar() && lastDeactivateTime_ > 0 &&
                         (now(CLOCK_MONOTONIC) - lastDeactivateTime_) < 500000;

    if (!spuriousCycle) {
      // Capture BEFORE the reset: if the engine's tracking says the bar
      // was emptied (sentinel ≤ 0) when we left, the next focus starts
      // with a known-empty bar — allow first-word FullReplace without
      // caret evidence.  Otherwise the bar probably holds a URL.
      bool leftBarEmpty = committedLen_ <= 0;
      viet_.reset();
      committedLen_ = 0;
      surroundingTextFailed_ = false; // fresh focus, re-verify
      surroundingInvalidCount_ = 0;
      addrBarIsFirstWord_ = true;
      addrBarHadSpace_ = false;
      // Genuine focus change (cross-app or ≥500ms away): the omnibox
      // content is no longer tracked — it almost always holds the page
      // URL or earlier text.  Block first-word FullReplace until caret
      // evidence proves the typed word replaced a selection — unless we
      // KNOW the bar was emptied before leaving.
      addrBarContentUnknown_ = !leftBarEmpty;
      addrBarWordStartCaretX_ = -1;
      addrBarLeftEdgeCaretX_ = -1;
      addrBarHadFirstWord_ = false;
      addrBarDidFullReplace_ = false;
      addrBarKeepState_ = false;
      addrBarPrevCommittedLen_ = 0;
      addrBarClearedByCtrlKey_ = false;
    } else {
      SKEY_DEBUG() << "Activate: spurious cycle (unarmed), preserving"
                   << " firstWord=" << addrBarIsFirstWord_
                   << " hadSpace=" << addrBarHadSpace_ << " composed='"
                   << viet_.getComposed() << "'";
      // X11 only: Chrome's focus churn can fire this cycle on a FRESH
      // omnibox session with nothing being composed.  Preserving the
      // previous session's first-word flags then blocks the FullReplace
      // autofill dismissal for the first word ("aâ" corruption on
      // Fedora).  With an empty composition there is nothing to
      // preserve — reset the first-word tracking so the next word gets
      // FullReplace.  Wayland keeps the old behavior: its autofill
      // handling goes through isAutofillCertain() (surrounding text),
      // not these flags, and resetting them mid-session broke the
      // first-word retype flow there.
      if (!isWayland() && viet_.getRawInput().empty()) {
        addrBarIsFirstWord_ = true;
        addrBarHadSpace_ = false;
        addrBarHadFirstWord_ = false;
        addrBarDidFullReplace_ = false;
        addrBarKeepState_ = false;
        addrBarPrevCommittedLen_ = 0;
        addrBarContentUnknown_ = false;
        addrBarWordStartCaretX_ = -1;
        addrBarLeftEdgeCaretX_ = -1;
      }
    }
  }
  clearLastWord();
  modeMenuActive_ = false;
  deferredCommitTimer_.reset();
  deferredCommitText_.clear();
  deferredPrefix_.clear();

  // Load per-app mode preference / exclusion, keyed by the resolved app
  // name (X11 WM_CLASS fallback for empty-program IBus-frontend apps).
  hasAppModeOverride_ = false;
  appExcluded_ = false;
  {
    RawConfig cfg;
    readAsIni(cfg, "conf/skey-app-modes.conf");
    auto *val = cfg.valueByPath(appProgram());
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
  cachedIsTerminalApp_ = -1;
  // A bare-caps decision belongs to the previous focus session.  A stale
  // pending/deadline must not leak into the new window — detectAutoMode
  // would otherwise lock sticky instantly on the first keystroke if the old
  // deadline already passed, and the non-Chromium fall-through could
  // inherit it.  Every focus gets its own full 2s decision window.
  modeDecisionPending_ = false;
  modeDecisionDeadlineUsec_ = 0;

  auto caps = ic_->capabilityFlags();

  // Engine-level sticky Uinput: if a previous IC for this Chromium program
  // reported bare caps (0x72), keep Uinput for subsequent ICs of the same
  // program — but only when the current caps lack "strong" content hints
  // (Alpha, SpellCheck, etc.) that indicate a real editor like Facebook chat.
  // A fresh AT-SPI2 web-editor focus means the user just clicked a real
  // editor: don't pre-lock Uinput here — detectAutoMode() bypasses the
  // sticky flag on the first keystroke (a11yFreshWebEditor) and clears it.
  if (engine_->chromiumHadBareCaps_ &&
      appProgram() == engine_->chromiumBareCapsProgram_ &&
      !appProgram().empty() && isChromiumCached() && !a11yFreshWebEditor()) {
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
               << " app=" << appProgram() << " caps=0x" << std::hex
               << static_cast<uint64_t>(caps.toInteger()) << std::dec
               << " cursor=(" << ic_->cursorRect().left() << ","
               << ic_->cursorRect().top() << "," << ic_->cursorRect().width()
               << "x" << ic_->cursorRect().height() << ")";
}

// Connect to a unix socket: filesystem path or abstract name (abstract =
// NUL-prefixed sun_path).
static bool connectUnixSocket(int fd, const std::string &path, bool abstract) {
  if (path.size() >= sizeof(((sockaddr_un *)0)->sun_path)) {
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  size_t len = path.size();
  if (abstract) {
    addr.sun_path[0] = '\0';
    memcpy(&addr.sun_path[1], path.c_str(), len);
    // len now counts the leading NUL: the address is exactly these bytes.
    // Do NOT add a further "+1" — the kernel compares abstract names byte
    // for byte up to the given length, and one stray trailing NUL makes
    // every connect fail with ECONNREFUSED against the server's bind
    // (which is len = offsetof + name.size() + 1, i.e. NUL + name, no
    // extra terminator).  Verified with a standalone repro, 2026-08-21.
    len += 1;
    socklen_t slen = offsetof(sockaddr_un, sun_path) + len;
    return connect(fd, reinterpret_cast<sockaddr *>(&addr), slen) == 0;
  }
  memcpy(addr.sun_path, path.c_str(), len);
  // Pathname sockets: include the terminating NUL, as usual.
  socklen_t slen = offsetof(sockaddr_un, sun_path) + len + 1;
  return connect(fd, reinterpret_cast<sockaddr *>(&addr), slen) == 0;
}

// The peer must be the real uinput server.  Accepted peers:
//  - our own uid  — the old hardened server that dropped privileges to us
//  - root (0)     — the legacy root server
//  - skey_uinput  — the current server, which runs as its own dedicated
//                   sysuser from start (never root, never drops)
// Anything else — a squatter on the abstract name, another user's process —
// is refused.
static bool peerIsTrusted(int fd) {
  ucred cred{};
  socklen_t credLen = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0) {
    SKEY_DEBUG() << "Uinput: probe SO_PEERCRED failed: " << strerror(errno);
    return false;
  }
  if (cred.uid == getuid() || cred.uid == 0) {
    return true;
  }
  // Resolve the sysuser uid once (getpwnam is cheap; the result is cached).
  static uid_t sysUserUid = [] {
    passwd pw{};
    passwd *result = nullptr;
    std::vector<char> buf(16384);
    if (getpwnam_r("skey_uinput", &pw, buf.data(), buf.size(), &result) == 0 &&
        result != nullptr) {
      return result->pw_uid;
    }
    return static_cast<uid_t>(-1);
  }();
  if (sysUserUid != static_cast<uid_t>(-1) && cred.uid == sysUserUid) {
    return true;
  }
  SKEY_DEBUG() << "Uinput: probe peer uid " << cred.uid
               << " untrusted (self=" << getuid()
               << " sysuser=" << static_cast<long>(sysUserUid) << ")";
  return false;
}

// One-shot probe connect: fs socket first, legacy abstract name second.
// Returns a connected+trusted fd or -1.  Never caches — callers decide
// whether to keep the fd (connectUinputServer) or discard it.
static int probeUinputServer() {
  auto tryConnect = [](const std::string &path, bool abstract) -> int {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      SKEY_DEBUG() << "Uinput: socket failed: " << strerror(errno);
      return -1;
    }
    if (!connectUnixSocket(fd, path, abstract)) {
      SKEY_DEBUG() << "Uinput: probe connect failed ("
                   << (abstract ? "abstract" : "fs") << "): " << strerror(errno);
      close(fd);
      return -1;
    }
    if (!peerIsTrusted(fd)) {
      close(fd);
      return -1;
    }
    return fd;
  };

  UinputSocketPaths paths = uinputSocketPaths("kb_socket");
  if (!paths.fsPath.empty()) {
    int fd = tryConnect(paths.fsPath, /*abstract=*/false);
    if (fd >= 0) {
      return fd;
    }
  }
  if (!paths.abstractName.empty()) {
    int fd = tryConnect(paths.abstractName, /*abstract=*/true);
    if (fd >= 0) {
      return fd;
    }
  }
  return -1;
}

bool SKeyState::connectUinputServer() {
  if (uinputClientFd_ >= 0) {
    return true;
  }
  int fd = probeUinputServer();
  if (fd < 0) {
    SKEY_DEBUG() << "Uinput: server unavailable";
    return false;
  }
  uinputClientFd_ = fd;
  SKEY_DEBUG() << "Uinput: connected";
  return true;
}

void SKeyState::sendBackspaceUinput(int count, uint32_t flags) {
  if (count < 0) {
    return;
  }
  if (count == 0 && flags == 0) {
    return;
  }
  if (!connectUinputServer()) {
    SKEY_DEBUG() << "Uinput: cannot send BS, server unavailable";
    return;
  }

  // Protocol v2: int32_t count, uint32_t flags, uint32_t textLen, then text.
  // textLen is always 0 — replacement text is committed via
  // ic_->commitString() (see handlePendingUinputBackspace), never typed
  // through uinput.  flags bit 0: send Escape before BS (deprecated —
  //   autocomplete is now handled via extra BS when isAutofillCertain()
  //   detects a selection).
  // The server detects v1 vs v2 by message size for backward compatibility.
  int32_t count32 = count;
  uint32_t textLen = 0;
  std::vector<char> msg(sizeof(int32_t) + sizeof(uint32_t) * 2);
  memcpy(msg.data(), &count32, sizeof(count32));
  memcpy(msg.data() + sizeof(count32), &flags, sizeof(flags));
  memcpy(msg.data() + sizeof(count32) + sizeof(flags), &textLen,
         sizeof(textLen));

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
  SKEY_DEBUG() << "Uinput: sent BS=" << count;
}

bool SKeyState::handlePendingUinputBackspace(KeyEvent &keyEvent) {
  // Late BS loopbacks from a force-committed replacement are swallowed:
  // their deletions may still be in flight in the app, and they must not
  // be treated as user backspaces (which would corrupt the screen and
  // the engine's word model).  Non-BS keys during the grace window fall
  // through to normal handling.
  if (uinputLateBsDeadlineUsec_ != 0) {
    if (now(CLOCK_MONOTONIC) < uinputLateBsDeadlineUsec_) {
      if (keyEvent.key().check(FcitxKey_BackSpace)) {
        SKEY_DEBUG() << "Uinput: swallow late loopback BS";
        keyEvent.filterAndAccept();
        return true;
      }
    } else {
      uinputLateBsDeadlineUsec_ = 0;
    }
  }

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

  int realBs = expectedUinputBackspaces_;
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
    } else if (isWayland()) {
      // Native Wayland apps: the commit delay scales with the number of
      // deletions (see kWaylandNativeCommitDelayPerBsUsec) — keep a small
      // base and a generous cap.
      minDelay = 4000;
      maxDelay = kWaylandNativeCommitDelayMaxUsec;
    }
  }
  uint64_t sleepUsec = std::clamp(static_cast<uint64_t>(bsRtEwma_ * multiplier),
                                  minDelay, maxDelay);

  if (uinputLoopbackSlow_) {
    // Previous replacement's loopbacks were slow — give the app extra
    // headroom before the commit so the BS are processed first.
    sleepUsec = std::max(sleepUsec, kSlowLoopbackCommitDelayFloorUsec);
    uinputLoopbackSlow_ = false;
  }

  if (realBs > 0 && !isChromiumCached() && isWayland()) {
    // Scale the headroom with the number of deletions: each injected BS
    // sits in the app's key queue and needs time to drain before the
    // commit lands on top of it.
    sleepUsec = std::max(sleepUsec, static_cast<uint64_t>(realBs) *
                                        kWaylandNativeCommitDelayPerBsUsec);
  }

  // Terminal exclusion uses the NAME list + cap bit only: the shell-scan
  // would flag IDEs with embedded terminals (antigravity, VS Code) as
  // terminals and drop them out of slow mode.
  // Wayland only: on X11 the sync anchor is a true barrier (X server
  // serializes key delivery), so the fast adaptive timing is already
  // safe and the 20ms slow-mode sleep would only add latency.
  // Wayland only: on X11 the sync anchor is a true barrier (X server
  // serializes key delivery), so the fast adaptive timing is already
  // safe and the 20ms slow-mode sleep would only add latency.
  // Standalone Electron apps (antigravity): their multi-process key
  // queue lags far behind the anchor loopback.  Terminals and the
  // address bar (own machinery) are excluded.
  bool slowMode = isWayland() && isChromiumCached() &&
                  !isChromiumBrowser(appProgram()) &&
                  !isTerminalAppName(appProgram()) &&
                  !ic_->capabilityFlags().test(CapabilityFlag::Terminal) &&
                  !inChromiumAddressBar();
  if (slowMode) {
    sleepUsec = std::max(sleepUsec, kUinputSlowModeSleepUsec);
  }

  SKEY_DEBUG() << "Uinput: sync BS, RT " << (elapsed / 1000) << "ms (ewma "
               << (bsRtEwma_ / 1000) << "ms), sleep " << (sleepUsec / 1000)
               << "ms then commit '" << commitText << "'"
               << (inChromiumAddressBar() ? " [addrbar]" : "")
               << (isChromiumCached() ? " [chromium]" : "")
               << (slowMode ? " [slow]" : "");

  usleep(sleepUsec);

  if (slowMode) {
    // Bounded verification: the app's surrounding cursor must have
    // reached the expected post-BS length before the commit; give it a
    // few short retries otherwise (3 × 2ms).
    for (int retry = 0; retry < kUinputSlowModeVerifyRetries; ++retry) {
      const auto &surr = ic_->surroundingText();
      if (surr.isValid() && static_cast<int>(surr.cursor()) == committedLen_)
        break;
      usleep(kUinputSlowModeRetryIntervalUsec);
    }
  }

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
      // ASCII control keys from Ctrl+combos (Ctrl+A select-all, Ctrl+U
      // clear-line...) mutate the bar in ways the engine cannot track.
      // On X11, reset the first-word model so the next word gets
      // FullReplace — the extra BS dismisses the autofill that
      // re-appears after the mutation ("chaào" corruption).  Function
      // keys (arrows, Home/End, sym > 0x20) are excluded: they move
      // the caret without changing the text before it.
      if (inChromiumAddressBar() && !isWayland() && sym < 0x20) {
        addrBarContentUnknown_ = false;
        addrBarHadSpace_ = false;
        addrBarHadFirstWord_ = false;
        committedLen_ = -1;
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

    // Ghost-composition guard (addr-bar, Uinput): the bar is provably
    // empty but a composition is still alive — a BackSpace during
    // Chrome's focus churn deleted the screen char without reaching the
    // engine (retyping "aa" after deleting "chào" produced "aâ").
    // Reset so the retype starts clean.  The empty-bar signal is
    // platform-specific: X11 uses the committedLen_ sentinel (-1),
    // Wayland uses the surrounding text (valid and empty).
    if (inChromiumAddressBar() && useUinputMode() &&
        !viet_.getRawInput().empty()) {
      bool barEmpty = false;
      if (isWayland()) {
        const auto &surr = ic_->surroundingText();
        barEmpty = surr.isValid() && surr.text().empty();
      } else {
        barEmpty = committedLen_ == -1;
      }
      if (barEmpty) {
        SKEY_DEBUG()
            << "AddrBar: ghost composition over empty bar, resetting";
        viet_.reset();
        committedLen_ = 0;
        addrBarSawBsInWord_ = false;
        addrBarHadFirstWord_ = false;
      }
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
  // Stop a11y snapshot polling — re-enabled on the next addrbar key.
  if (auto *mon = engine_->a11yMonitor()) {
    mon->setPollingEnabled(false);
  }

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
                preeditPendingProgram_ = appProgram();
                engine_->pendingPreedits_[appProgram()] = viet_.getComposed();
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
      preeditPendingProgram_ = appProgram();
      engine_->pendingPreedits_[appProgram()] = viet_.getComposed();
      SKEY_DEBUG() << "Deactivate: saved preedit '" << viet_.getComposed()
                   << "' for program '" << appProgram() << "'";
    }
  }
  viet_.reset();
  committedLen_ = 0;
  clearLastWord();
  clearUI();
}

void SKeyState::reset() {
  SKEY_DEBUG() << "Reset: entered uinputFwd=" << uinputKeyForwarded_
               << " ffSnap=" << isFirefoxOrSnap() << " prog=" << appProgram();
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
    preeditPendingProgram_ = appProgram();
    engine_->pendingPreedits_[appProgram()] = viet_.getComposed();
    SKEY_DEBUG() << "Reset: saved preedit '" << viet_.getComposed()
                 << "' for program '" << appProgram() << "'";
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
  cachedIsTerminalApp_ = -1;
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
  //
  // Moving focus within the same window (chat → Google Sheets cell) does
  // NOT create a new input context, so activate() never runs and the cached
  // mode would carry over — force a re-evaluation when the a11y monitor
  // shows the focus is not on a text entry and the cached mode isn't
  // Uinput yet.
  bool a11yNonEntry = a11yBrowserNonEntry();
  if (viet_.getRawInput().empty() &&
      (modeDecisionPending_ || a11yFreshWebEditor() ||
       (a11yNonEntry &&
        (!modeCacheValid_ || cachedMode_ != SKeyOutputMode::Uinput)))) {
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
       programIsLockScreen(appProgram()))) {
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

  // Enable the a11y snapshot polling only while typing in the Chromium
  // address bar on X11 (see A11yMonitor::setPollingEnabled) — polling
  // any other focused entry is wasted DBus traffic.
  if (auto *mon = engine_->a11yMonitor()) {
    mon->setPollingEnabled(!isWayland() && inChromiumAddressBar());
  }

  // Late uinput BS loopbacks — BS we injected that arrive after the
  // deletion window closed (slow apps).  Swallow them: treating them as
  // fresh user backspaces would pop composition chars and forward stray
  // deletions to the app.
  if (uinputBsOutstanding_ > 0 && keyEvent.key().check(FcitxKey_BackSpace)) {
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
      engine_->saveAppExcluded(appProgram(), false);
      appModeOverride_ = newMode;
      hasAppModeOverride_ = true;
      modeCacheValid_ = false;
      engine_->saveAppMode(appProgram(), newMode);
      SKEY_INFO() << "Mode switched to " << outputModeName(newMode);
      dismissModeMenu();
      keyEvent.filterAndAccept();
      return;
    } else if (choice == 5) {
      bool newExcluded = !appExcluded_;
      appExcluded_ = newExcluded;
      engine_->saveAppExcluded(appProgram(), newExcluded);
      SKEY_INFO() << "App '" << appProgram()
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
          SKEY_DEBUG() << "AutoRestore: '" << preRestore << "' -> '"
                       << postRestore << "'";
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
      // X11 delivers the RAW sym here (0x61 for Ctrl+A, not 0x01), so
      // detect Ctrl+letter via the states mask.  Ctrl+A (select-all),
      // Ctrl+U (clear-line) and Ctrl+L (omnibox+select-all) replace-or-
      // clear the whole bar: the next word is provably the only content
      // before the caret — arm the one-shot cleared-bar flag and the -1
      // sentinel so first-word FullReplace dismisses the inline-autofill
      // selection that re-appears on retype ("chaào" corruption).
      // Other Ctrl+letters (C/V/X/Z…) mutate the bar unpredictably —
      // drop the flag, keep the plain 0 reset.
      bool ctrlClearsBar =
          !isWayland() && key.states().test(KeyState::Ctrl) &&
          (sym == 'a' || sym == 'A' || sym == 'u' || sym == 'U' ||
           sym == 'l' || sym == 'L');
      addrBarClearedByCtrlKey_ = ctrlClearsBar;
      if (ctrlClearsBar) {
        addrBarContentUnknown_ = false;
        addrBarHadSpace_ = false;
        committedLen_ = -1;
      } else {
        committedLen_ = 0;
      }
    }
    return;
  }

  // Handle Backspace while composing
  if (key.check(FcitxKey_BackSpace) && !viet_.getRawInput().empty()) {
    // Chromium address bar: pass raw BS through to Chrome (X11) instead
    // of sending forwardKey via D-Bus (which triggers focus changes).
    // Just update bamboo state and let the keystroke reach the app.
    if (inChromiumAddressBar()) {
      // A backspace may desync the engine from the screen (autofill can
      // eat the BS) — arm the a11y desync guard for the next keys.
      addrBarSawBsInWord_ = true;
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
      size_t preLen = utf8::length(viet_.getComposed());
      viet_.backspace();
      if (!viet_.getRawInput().empty() &&
          utf8::length(viet_.getComposed()) == preLen) {
        // The popped keystroke was a tone marker — one BS deletes the
        // whole accented SCREEN char ("câ" → "c"), so pop the base
        // letter too or the model drifts ahead of the screen.
        viet_.backspace();
      }
      // X11: empty bar → sentinel -1 so the next word's snapshot
      // (addrBarPrevCommittedLen_ < 0) re-arms first-word FullReplace.
      // Wayland keeps 0 — its tracking uses the surrounding text.
      if (viet_.getRawInput().empty()) {
        committedLen_ = isWayland() ? 0 : -1;
      } else {
        committedLen_ =
            static_cast<int>(utf8::length(viet_.getComposed()));
      }
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
          while (last > 0 && isUtf8ContinuationByte(appText[last]))
            --last;
          appText.resize(last);
        }
        if (viet_.getComposed() != appText) {
          SKEY_DEBUG() << "SurrBS: uinput follow app, raw='" << appText << "'";
          viet_.setRawInput(appText);
        }
        committedLen_ =
            viet_.getRawInput().empty()
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
        while (last > 0 && isUtf8ContinuationByte(appText[last]))
          --last;
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
      // A backspace may desync the engine from the screen — arm the
      // a11y desync guard for the next keys.
      addrBarSawBsInWord_ = true;
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
        // Deleting past the tracked text: the screen holds content the
        // engine lost (keystrokes dropped during Chrome's focus churn).
        // Mark tracking unknown so the next replacement uses the safe
        // plain path instead of FullReplace ("chàobạn" corruption).
        addrBarContentUnknown_ = true;
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
          SKEY_DEBUG() << "AutoRestore: '" << preRestore << "' -> '"
                       << postRestore << "'";
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
            uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec, 0,
                [this](EventSourceTime *, uint64_t) {
                  SKEY_DEBUG() << "AutoRestore: safety timeout, force commit";
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
          SKEY_DEBUG() << "AutoRestore: '" << preRestore << "' -> '"
                       << postRestore << "'";
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
            uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec, 0,
                [this](EventSourceTime *, uint64_t) {
                  SKEY_DEBUG() << "AutoRestore: safety timeout, force commit";
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
      addrBarSawBsInWord_ = false;
      addrBarClearedByCtrlKey_ = false;
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
          SKEY_DEBUG() << "AutoRestore: '" << preRestore << "' -> '"
                       << postRestore << "'";
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
            uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec, 0,
                [this](EventSourceTime *, uint64_t) {
                  SKEY_DEBUG() << "AutoRestore: safety timeout, force commit";
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

      // Desync guard (Chromium address bar, X11): if the bar's text no
      // longer contains the word the engine is tracking (e.g. Chrome's
      // autofill ate a user backspace during deletion, leaving a stale
      // composition), reset the composition so new keystrokes start
      // from the screen's real state instead of appending to a ghost
      // word.  Gated on addrBarSawBsInWord_ — only backspace-driven
      // edits can desync, and the snapshot may briefly lag a fresh
      // commit (which would otherwise cause a false reset).
      if (inChromiumAddressBar() && !isWayland() && useUinputMode() &&
          addrBarSawBsInWord_ && !viet_.getRawInput().empty()) {
        std::string comp = viet_.getComposed();
        std::string txt;
        int ss = -1, se = -1;
        auto *mon = engine_->a11yMonitor();
        uint64_t waitUntil = now(CLOCK_MONOTONIC) + 30000;
        for (;;) {
          if (!mon || !mon->a11yState(txt, ss, se, kA11ySnapshotMaxAgeUsec))
            break;
          if (txt.find(comp) != std::string::npos)
            break; // word still on screen — in sync
          uint64_t remaining = waitUntil - now(CLOCK_MONOTONIC);
          if (now(CLOCK_MONOTONIC) >= waitUntil)
            break;
          mon->waitForSnapshotUpdate(remaining);
        }
        // An EMPTY snapshot is not desync evidence — Chrome on some
        // distros (Fedora) returns an empty omnibox text while the word
        // is clearly on screen; resetting on it breaks every retype
        // after a backspace.
        if (mon && !txt.empty() && txt.find(comp) == std::string::npos) {
          SKEY_DEBUG() << "AddrBar: desync — bar text '" << txt
                       << "' lacks composed '" << comp << "', resetting";
          viet_.reset();
          committedLen_ = -1;
          addrBarContentUnknown_ = true;
          addrBarSawBsInWord_ = false;
        }
      }

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
              // Caret X before this word's first key reaches Chrome —
              // the caret-jump check compares against it to detect a
              // replaced selection (Ctrl+L + type) after a genuine
              // focus change.
              addrBarWordStartCaretX_ = ic_->cursorRect().left();
              {
                int caretX = addrBarWordStartCaretX_;
                if (caretX > 0 &&
                    (addrBarLeftEdgeCaretX_ < 0 ||
                     caretX < addrBarLeftEdgeCaretX_)) {
                  addrBarLeftEdgeCaretX_ = caretX;
                }
              }
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
                SKEY_DEBUG() << "AddrBar: consume routing → schedule (sentinel="
                             << addrBarPrevCommittedLen_
                             << " hadFirst=" << addrBarHadFirstWord_ << ")";
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
                    static_cast<int>(sym), newComposed, oldAscii, oldComposed);
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
          SKEY_DEBUG() << "AutoRestore: '" << preRestore << "' -> '"
                       << postRestore << "'";
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
            uinputSafetyTimer_ = engine_->instance()->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC,
                now(CLOCK_MONOTONIC) + uinputTiming().safetyTimeoutUsec, 0,
                [this](EventSourceTime *, uint64_t) {
                  SKEY_DEBUG() << "AutoRestore: safety timeout, force commit";
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
      addrBarSawBsInWord_ = false;
      addrBarClearedByCtrlKey_ = false;
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
    addrBarSawBsInWord_ = false;
  } else {
    // Non-composing key (Home, End, etc.) invalidates retroactive editing
    clearLastWord();
  }
  // Caret-moving keys (arrows, Home/End, Delete…) invalidate the
  // cleared-by-Ctrl-key guarantee — the caret may now sit mid-text.
  // Escape is exempt: it only dismisses the autocomplete dropdown.
  if (sym != FcitxKey_Escape) {
    addrBarClearedByCtrlKey_ = false;
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
                                           bool oldComposedIsAscii,
                                           const std::string &oldComposed) {
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
      // ── X11: AT-SPI2 word-at-start check, heuristic fallback ──
      // Chrome's a11y tree exposes the omnibox's text INCLUDING inline
      // autofill (which is appended after the caret and selected).
      // When the composed word sits at the START of the bar text,
      // nothing exists before the caret — FullReplace (delete whole
      // word + 1 BS) is provably safe: the extra BS either eats the
      // autofill selection or is a no-op, and any suffix after the
      // caret is untouched because BS only deletes backwards.
      // Otherwise the word sits after existing text (URL etc.) and
      // plain replacement with the exact BS count is used.
      // When AT-SPI2 is unavailable, fall back to the first-word
      // FullReplace heuristics below.
      bool a11yDecided = false;
      bool wordAtStart = false;
      if (!oldComposed.empty()) {
        auto *mon = engine_->a11yMonitor();
        std::string a11yText;
        int a11ySelStart = -1, a11ySelEnd = -1;
        // Wait briefly for a snapshot that includes all forwarded keys
        // (the monitor re-polls on text-change signals, so it catches
        // up within ~30ms).  The a11y verdict is authoritative ONLY for
        // the positive case (text provably starts with the composed
        // word): Chrome on some distros (Fedora) returns a FRESH but
        // EMPTY omnibox snapshot, and treating that as "word not at
        // start" vetoes the first-word FullReplace that dismisses
        // autofill ("aâ" corruption).  Empty/stale/timeout snapshots
        // fall through to the first-word heuristics below.
        uint64_t waitUntil = now(CLOCK_MONOTONIC) + 30000;
        for (;;) {
          if (!mon || !mon->a11yState(a11yText, a11ySelStart, a11ySelEnd,
                                      kA11ySnapshotMaxAgeUsec))
            break;
          if (a11yText.size() >= oldComposed.size() &&
              a11yText.compare(0, oldComposed.size(), oldComposed) == 0) {
            wordAtStart = true;
            a11yDecided = true;
            break;
          }
          if (now(CLOCK_MONOTONIC) >= waitUntil)
            break; // timeout — heuristics decide
          mon->waitForSnapshotUpdate(waitUntil - now(CLOCK_MONOTONIC));
        }
      }
      if (wordAtStart) {
        totalBs = oldComposedLen + 1;
        commitText = fullComposed;
        {
          std::string preRestore = commitText;
          viet_.autoRestore();
          std::string postRestore = viet_.getComposed();
          if (preRestore != postRestore) {
            SKEY_DEBUG() << "AddrBar: autoRestore '" << preRestore << "' -> '"
                         << postRestore << "'";
            commitText = postRestore;
          }
        }
        addrBarHadFirstWord_ = true;
        addrBarDidFullReplace_ = !(oldComposedIsAscii && oldComposedLen == 1);
        addrBarKeepState_ = (oldComposedIsAscii && oldComposedLen == 1);
        SKEY_DEBUG() << "AddrBar: a11y word-at-start, fullReplace BS="
                     << totalBs << " commit='" << commitText << "'"
                     << (addrBarKeepState_ ? " [keep-state]" : "");
      } else if (!a11yDecided) {
        SKEY_DEBUG() << "AddrBar: fallback snapshot="
                     << addrBarPrevCommittedLen_
                     << " hadFirst=" << addrBarHadFirstWord_
                     << " oldLen=" << oldComposedLen;
        // Only the first word after focus gets FullReplace (oldComposedLen
        // + 1 BS to dismiss Chrome autocomplete).  Subsequent words use
        // plain replacement (exact BS count, no Escape) — the forwarded
        // matching-append keys already race on X11, and adding Escape or
        // extra BS only makes the race condition worse.
        //
        // addrBarPrevCommittedLen_ is a snapshot of committedLen_ before the
        // current replacement.  If < 0 (sentinel -1 from backspacing past
        // all tracked text), tracking is lost — reset the first-word flag
        // so FullReplace can fire again when safe.  Do NOT reset
        // addrBarHadSpace_: a committed space means text may still exist
        // before the cursor even when tracking is negative (e.g. "xin"
        // survives after deleting "chào" from "xin chào ") — resetting it
        // would let the next FullReplace delete the space and join words.
        if (addrBarPrevCommittedLen_ < 0) {
          addrBarHadFirstWord_ = false;
        }
        if (!addrBarHadFirstWord_ && oldComposedLen > 0 &&
            !fullComposed.empty()) {
          bool hasTextBefore = false;
          if (addrBarClearedByCtrlKey_) {
            // Ctrl+A/U/L cleared the bar immediately before this word —
            // nothing exists before the cursor.  Skip the evidence
            // chain: the pre-typing caret sat at the END of the old
            // selection, so word-start X is far right of the left edge
            // and the edge check would wrongly block FullReplace.
            addrBarClearedByCtrlKey_ = false; // one-shot
          } else {
            const auto &surrounding = ic_->surroundingText();
            if (surrounding.isValid()) {
              hasTextBefore = surrounding.cursor() >
                              static_cast<unsigned int>(oldComposedLen);
            } else if (addrBarPrevCommittedLen_ < 0) {
              // Sentinel: the engine's tracked text is empty.  Whether the
              // BAR is empty is decided by the caret: a word starting at
              // the left edge (min caret X seen this session) means the
              // bar was empty — FullReplace is safe; further right means
              // text exists before the cursor (partial deletion) and the
              // +1 BS would eat it — use the plain path.
              hasTextBefore =
                  addrBarLeftEdgeCaretX_ > 0 && addrBarWordStartCaretX_ > 0 &&
                  addrBarWordStartCaretX_ > addrBarLeftEdgeCaretX_ + 30;
            } else if (addrBarHadSpace_ || addrBarPrevCommittedLen_ > 0) {
              // A committed space or tracked chars before this word mean
              // text exists before the cursor → FullReplace's extra BS
              // (oldComposedLen + 1) would delete it.
              hasTextBefore = true;
            } else {
              // Caret evidence is the primary signal: if the caret jumped
              // far LEFT since the word started, the typed word replaced a
              // selection (Ctrl+A / Ctrl+L + type) — the bar before the
              // word is empty, so FullReplace is safe.  Without a jump,
              // only block when tracking is known to be lost (genuine
              // focus onto a bar that likely holds a URL).  A stale or
              // invalid rect (<= 0) is treated as unsafe.
              static constexpr int kCaretJumpPx = 40;
              int caretX = ic_->cursorRect().left();
              bool jumpedLeft = addrBarWordStartCaretX_ > 0 && caretX > 0 &&
                                caretX + kCaretJumpPx < addrBarWordStartCaretX_;
              if (!jumpedLeft && addrBarContentUnknown_) {
                hasTextBefore = true;
              }
            }
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
            addrBarKeepState_ = (oldComposedIsAscii && oldComposedLen == 1);
            SKEY_DEBUG() << "AddrBar: first word, fullReplace BS=" << totalBs
                         << " commit='" << commitText << "'"
                         << (addrBarKeepState_ ? " [keep-state]" : "");
          }
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
              surrounding.cursor() > static_cast<unsigned int>(oldComposedLen);
        }
        if (!hasTextBefore) {
          totalBs = oldComposedLen + 1;
          commitText = fullComposed;
          {
            std::string preRestore = commitText;
            viet_.autoRestore();
            std::string postRestore = viet_.getComposed();
            if (preRestore != postRestore) {
              SKEY_DEBUG() << "AddrBar: autoRestore '" << preRestore << "' -> '"
                           << postRestore << "'";
              commitText = postRestore;
            }
          }
          addrBarHadFirstWord_ = true;
          addrBarDidFullReplace_ =
              !(oldComposedIsAscii && oldComposedLen == 1);
          addrBarKeepState_ = (oldComposedIsAscii && oldComposedLen == 1);
          SKEY_DEBUG() << "AddrBar: first word, fullReplace BS=" << totalBs
                       << " commit='" << commitText << "'"
                       << (addrBarKeepState_ ? " [keep-state]" : "");
        }
      } else if (isAutofillCertain()) {
        ++totalBs;
        SKEY_DEBUG() << "AddrBar: autofill detected, +1 BS (total=" << totalBs
                     << ")";
      }
    }

    // Trigger-key guard: prevent Chrome's re-delivered key (from focus
    // cycle after commitString) from being processed as a second key press.
    // Only set when triggerKeySym is non-zero — the SurroundingText path
    // already sets the guard before calling us (with the correct sym) and
    // passes triggerKeySym=0 here; we must not overwrite that with 0.
    if (triggerKeySym != 0) {
      addrBarLastTriggerKey_ = triggerKeySym;
      addrBarTriggerDeadline_ =
          now(CLOCK_MONOTONIC) + (isWayland() ? 100000 : 200000);
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
    sendBackspaceUinput(totalBs + 1, uinputFlags); // +1 sync BS
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
              deleteLen, addedPart, static_cast<int>(utf8::length(oldComposed)),
              0, newComposed, false, oldComposed);
          return;
        }
        if (useUinputMode()) {
          sendBackspaceUinput(deleteLen + 1); // +1 sync BS
          expectedUinputBackspaces_ = deleteLen;
          seenUinputBackspaces_ = 0;
          pendingUinputCommit_ = addedPart;
          uinputPendingFinalLen_ = newLen;
          uinputDeleting_ = true;
          // Safety: force-commit if BS events are lost.  Uses the shared
          // armUinputSafetyTimer() so the slow-loopback extension and the
          // late-loopback grace apply here too (Telegram on Wayland can
          // stall past the base window).
          armUinputSafetyTimer();
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
          if (isWayland()) {
            // Wayland has no ordering guarantee between forwardKey (via
            // the compositor's virtual-keyboard protocol) and commitString
            // (via text-input) — an immediate commit races the forwarded
            // BackSpace keys and eats characters (observed in Chromium and
            // Telegram: "đâu" → "dau").  Commit only after the app has had
            // time to process them.  Pass the stable prefix (already on
            // screen after the BS deletes): follow-up keystrokes then
            // extend only the pending suffix, otherwise the whole word
            // would be re-committed over the prefix ("chào" → "chchào").
            scheduleDeferredCommit(addedPart, stablePrefix);
          } else {
            // X11: forwarded BS and commitString are serialized through
            // the X server, so committing immediately is safe.
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
            // Telegram...).  Chromium-family apps downgrade immediately;
            // others get one retry: deleting to empty makes the cache
            // transiently invalid and the app usually re-pushes it on the
            // next word — a hard downgrade would lock a healthy app into
            // Uinput for the whole focus session.
            if (noteSurroundingFailure()) {
              surroundingTextFailed_ = true;
              modeCacheValid_ = false;
              SKEY_DEBUG()
                  << "Surr: surrounding text invalid, downgrading to uinput";
            } else {
              SKEY_DEBUG()
                  << "Surr: surrounding text invalid, retrying next word";
            }
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
          surroundingInvalidCount_ = 0; // cache works again
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
      // Chromium-family apps downgrade immediately; others get one retry
      // (see noteSurroundingFailure).
      if (noteSurroundingFailure()) {
        surroundingTextFailed_ = true;
        modeCacheValid_ = false;
        SKEY_DEBUG()
            << "SurrBS: surrounding text invalid, downgrading to uinput";
      } else {
        SKEY_DEBUG() << "SurrBS: surrounding text invalid, retrying";
      }
      ic_->forwardKey(Key(FcitxKey_BackSpace));
    } else {
      ic_->deleteSurroundingText(-1, 1);
      if (ic_->surroundingText().isValid()) {
        ic_->surroundingText().deleteText(-1, 1);
      }
      surroundingInvalidCount_ = 0; // cache works again
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
  std::string userDir = userPkgDataDir();
  std::string path = userDir + "/skey/user-dict.txt";
  std::ifstream in(path);
  if (!in.is_open())
    return;
  std::string line;
  int count = 0;
  while (std::getline(in, line)) {
    size_t b = line.find_first_not_of(" \t\r\n");
    if (b == std::string::npos || line[b] == '#')
      continue;
    size_t e = line.find_last_not_of(" \t\r\n");
    std::string word = line.substr(b, e - b + 1);
    if (word.empty())
      continue;
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
          uinputLoopbackSlow_ = true;
          SKEY_DEBUG() << "Uinput: BS loopbacks slow — extend safety window";
          armUinputSafetyTimer();
          return true;
        }
        SKEY_DEBUG() << "Uinput: safety timeout, force commit";
        uinputSafetyRetried_ = false;
        uinputSafetyTimer_.reset();
        uinputCommitTimer_.reset();
        // The in-flight BS may still reach the app after this commit —
        // arm the late-loopback grace so their loopbacks are swallowed
        // instead of being treated as user backspaces, and drop word
        // tracking since the screen state is now uncertain.
        uinputLateBsDeadlineUsec_ = now(CLOCK_MONOTONIC) + 400000;
        std::string text = std::move(pendingUinputCommit_);
        pendingUinputCommit_.clear();
        expectedUinputBackspaces_ = 0;
        seenUinputBackspaces_ = 0;
        uinputBsOutstanding_ = 0;
        uinputDeleting_ = false;
        clearLastWord();
        committedLen_ = 0;
        uinputPendingFinalLen_ = 0;
        if (!text.empty())
          this->commitText(text);
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
