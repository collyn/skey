#ifndef SKEY_PROCESS_MARKERS_H
#define SKEY_PROCESS_MARKERS_H

// Binary-layout markers of the Chromium and Gecko runtimes, shared by the
// engine and the accessibility monitor.  `root` prefixes file probes so a
// sandboxed process (flatpak: /proc/<pid>/exe names a path inside its own
// mount namespace) can be checked through /proc/<pid>/root.

#include <algorithm>
#include <cctype>
#include <climits>
#include <string>
#include <unistd.h>

namespace skey {

// Chromium-family browsers AND Electron apps: the exe path names the
// runtime, or its directory ships Chromium's helper binaries (renamed
// Electron shells such as antigravity).  Newer Electron builds renamed the
// crashpad handler to "browser_crashpad_handler" (ChatGPT on Mint ships
// only that one, 2026-09-08).
inline bool exeHasChromiumMarkers(const std::string &exe, const std::string &root = {}) {
    std::string lower = exe;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("electron") != std::string::npos ||
        lower.find("chrome") != std::string::npos ||
        lower.find("chromium") != std::string::npos)
        return true;
    const size_t lastSlash = exe.rfind('/');
    if (lastSlash == std::string::npos) return false;
    const std::string dir = root + exe.substr(0, lastSlash);
    return access((dir + "/chrome-sandbox").c_str(), F_OK) == 0 ||
           access((dir + "/chrome_crashpad_handler").c_str(), F_OK) == 0 ||
           access((dir + "/browser_crashpad_handler").c_str(), F_OK) == 0;
}

// Gecko browsers always ship libxul.so next to their main binary — a
// binary-layout check instead of hardcoding names (zen, librewolf, ...).
inline bool exeHasGeckoMarker(const std::string &exe, const std::string &root = {}) {
    const size_t lastSlash = exe.rfind('/');
    if (lastSlash == std::string::npos) return false;
    return access((root + exe.substr(0, lastSlash) + "/libxul.so").c_str(), F_OK) == 0;
}

// The exe of a known pid; empty when unreadable.
inline std::string processExe(pid_t pid) {
    char buf[PATH_MAX];
    const std::string link = "/proc/" + std::to_string(pid) + "/exe";
    const ssize_t len = readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (len <= 0) return {};
    buf[len] = '\0';
    return buf;
}

} // namespace skey

#endif // SKEY_PROCESS_MARKERS_H
