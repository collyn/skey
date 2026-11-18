#ifndef SKEY_GNOME_APP_IDENTITY_H
#define SKEY_GNOME_APP_IDENTITY_H

// Pure helpers for naming the app behind GNOME Shell's IBus relay.
//
// On GNOME Wayland every client reaches fcitx5 through GNOME Shell's input
// method bridge, so fcitx5 older than 5.1.22 reports each of them as
// "gnome-shell".  GnomeFocusWatcher recovers the focused app id from the
// shell's Introspect replies; this header holds the parts that need no bus:
// version gating, reply interpretation, call/reply pairing and turning a
// Shell app id ("firefox_firefox.desktop") into the program name the rest
// of skey classifies ("firefox").

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skey::gnome {

/// fcitx5 names relayed GNOME apps itself from 5.1.22 on (ibus frontend app
/// monitor).  Unparsable versions answer false: the relay-only substitution
/// in the engine keeps a real program name untouched either way.
inline bool fcitxNamesRelayedApps(const char *version) {
  if (!version) return false;
  int major = 0, minor = 0, patch = 0;
  const int fields = std::sscanf(version, "%d.%d.%d", &major, &minor, &patch);
  if (fields < 2) return false;
  if (major != 5) return major > 5;
  if (minor != 1) return minor > 1;
  return patch >= 22;
}

/// Program names fcitx5 gives the GNOME relay context.
inline bool isRelayProgram(std::string_view program) {
  return program.empty() || program == "gnome-shell";
}

/// One app of a GetRunningApplications reply: its id and whether any seat
/// reports it focused ("active-on-seats" non-empty).
struct RunningApp {
  std::string id;
  bool focused = false;
};

/// The focused app id, or empty when none is focused or the shell only has
/// a per-session window id ("window:N" — no .desktop file, never stable).
inline std::string focusedAppId(const std::vector<RunningApp> &apps) {
  for (const auto &app : apps) {
    if (!app.focused) continue;
    if (app.id.empty() || app.id.rfind("window:", 0) == 0) return {};
    return app.id;
  }
  return {};
}

/// Matches the portal's GetRunningApplications calls with GNOME Shell's
/// replies as seen by a bus monitor.  Names are unique bus names; serials
/// come straight from the message headers.
class IntrospectPairing {
public:
  void setOwners(const std::string &shell, const std::string &portal) {
    if (shell == shell_ && portal == portal_) return;
    shell_ = shell;
    portal_ = portal;
    pending_.clear();
  }
  void noteCall(const std::string &sender, uint32_t serial) {
    if (portal_.empty() || sender != portal_ || serial == 0) return;
    if (pending_.size() >= 16) pending_.erase(pending_.begin());
    pending_.push_back(serial);
  }
  /// True when the reply answers a call noted before; consumes that call.
  bool takeReply(const std::string &sender, const std::string &destination,
                 uint32_t replySerial) {
    if (shell_.empty() || sender != shell_ || destination != portal_) return false;
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (*it != replySerial) continue;
      pending_.erase(it);
      return true;
    }
    return false;
  }
  bool hasPendingCall() const { return !pending_.empty(); }

private:
  std::string shell_, portal_;
  std::vector<uint32_t> pending_;
};

/// Value of `key` in the [Desktop Entry] group of a .desktop file's text.
/// Localized variants (Key[xx]) are ignored.
inline std::optional<std::string> desktopEntryValue(std::string_view text,
                                                    std::string_view key) {
  bool inEntry = false;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
      line.remove_prefix(1);
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '[') {
      inEntry = line == "[Desktop Entry]";
      continue;
    }
    if (!inEntry) continue;
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view name = line.substr(0, eq);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
    if (name != key) continue;
    std::string_view value = line.substr(eq + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.remove_prefix(1);
    return std::string(value);
  }
  return std::nullopt;
}

/// Splits an Exec value into arguments (desktop-entry quoting: double
/// quotes, backslash escapes inside them).
inline std::vector<std::string> splitExec(std::string_view exec) {
  std::vector<std::string> args;
  std::string current;
  bool quoted = false, any = false;
  for (size_t i = 0; i < exec.size(); ++i) {
    const char c = exec[i];
    if (quoted) {
      if (c == '\\' && i + 1 < exec.size()) {
        current += exec[++i];
      } else if (c == '"') {
        quoted = false;
      } else {
        current += c;
      }
      continue;
    }
    if (c == '"') {
      quoted = any = true;
    } else if (c == ' ' || c == '\t') {
      if (any) args.push_back(std::move(current));
      current.clear();
      any = false;
    } else {
      current += c;
      any = true;
    }
  }
  if (any) args.push_back(std::move(current));
  return args;
}

inline std::string baseName(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// The program an Exec line starts: argv[0]'s basename after `env` and its
/// assignments, or the app id for `flatpak run`.  Empty when the line runs
/// a shell or launcher that says nothing about the app.
inline std::string execProgram(std::string_view exec) {
  const auto args = splitExec(exec);
  size_t i = 0;
  if (i < args.size() && baseName(args[i]) == "env") {
    ++i;
    while (i < args.size() &&
           (args[i].find('=') != std::string::npos || (!args[i].empty() && args[i][0] == '-'))) {
      // Options that take the next argument (env -u NAME, -C DIR, -S STRING).
      const bool takesValue = args[i] == "-u" || args[i] == "--unset" || args[i] == "-C" ||
                              args[i] == "--chdir" || args[i] == "-S" || args[i] == "--split-string";
      i += takesValue ? 2 : 1;
    }
  }
  if (i >= args.size()) return {};
  const std::string program = baseName(args[i]);
  if (program == "flatpak") {
    size_t j = i + 1;
    while (j < args.size() && args[j] != "run") ++j;
    for (++j; j < args.size(); ++j) {
      if (args[j].empty() || args[j][0] == '-' || args[j][0] == '@') continue;
      return args[j];
    }
    return {};
  }
  static constexpr std::string_view opaque[] = {"sh", "bash", "dash", "zsh", "gtk-launch",
                                                "gio", "xdg-open", "snap"};
  for (auto name : opaque)
    if (program == name) return {};
  if (program.empty() || program[0] == '%') return {};
  return program;
}

/// "firefox_firefox.desktop" → "firefox_firefox"; other ids lose ".desktop".
inline std::string desktopIdStem(std::string id) {
  constexpr std::string_view suffix = ".desktop";
  if (id.size() > suffix.size() &&
      id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0)
    id.resize(id.size() - suffix.size());
  return id;
}

/// Reads the text of the .desktop file for an id ("foo.desktop"), or
/// nullopt when there is none.
using DesktopFileReader = std::function<std::optional<std::string>(const std::string &id)>;

/// The program name skey uses for a Shell app id: what its .desktop file
/// launches (matching the names fcitx5 reports elsewhere — process names on
/// X11/KDE, the app id for flatpaks), else the id without ".desktop" and,
/// for snaps ("<snap>_<app>"), without the snap prefix.
inline std::string programForAppId(const std::string &appId, const DesktopFileReader &read) {
  if (appId.empty()) return {};
  if (read) {
    if (auto text = read(appId)) {
      if (auto exec = desktopEntryValue(*text, "Exec")) {
        std::string program = execProgram(*exec);
        if (!program.empty()) return program;
      }
    }
  }
  std::string stem = desktopIdStem(appId);
  const size_t underscore = stem.rfind('_');
  if (underscore != std::string::npos && underscore + 1 < stem.size() &&
      stem.find('.') == std::string::npos)
    stem.erase(0, underscore + 1);
  return stem;
}

/// Directories searched for applications/<id>, in XDG precedence order,
/// plus the snap and flatpak export dirs a daemon started outside the
/// session environment would miss.
inline std::vector<std::string> desktopFileDirs(const char *dataHome, const char *home,
                                                const char *dataDirs) {
  std::vector<std::string> dirs;
  auto add = [&](std::string dir) {
    if (dir.empty() || dir.front() != '/') return;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    for (const auto &known : dirs)
      if (known == dir) return;
    dirs.push_back(std::move(dir));
  };
  if (dataHome && *dataHome) add(dataHome);
  else if (home && *home) add(std::string(home) + "/.local/share");
  std::string list = dataDirs && *dataDirs ? dataDirs : "/usr/local/share:/usr/share";
  size_t pos = 0;
  while (pos <= list.size()) {
    size_t end = list.find(':', pos);
    if (end == std::string::npos) end = list.size();
    add(list.substr(pos, end - pos));
    pos = end + 1;
  }
  if (home && *home) add(std::string(home) + "/.local/share/flatpak/exports/share");
  add("/var/lib/flatpak/exports/share");
  add("/var/lib/snapd/desktop");
  return dirs;
}

} // namespace skey::gnome

#endif
