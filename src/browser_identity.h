#ifndef SKEY_BROWSER_IDENTITY_H
#define SKEY_BROWSER_IDENTITY_H

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>

namespace skey {
inline std::string browserProgramName(const std::string &program) {
    std::string name = program;
    // Some Xwayland/Chromium frontends expose the executable and its launch
    // flags as one program string (for example
    // `msedge --ozone-platform=x11 --gtk-version=4`).  Classification and
    // per-browser markers must use argv[0], never the command-line tail.
    const auto whitespace = name.find_first_of(" \t\r\n");
    if (whitespace != std::string::npos) name.resize(whitespace);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name.erase(0, slash + 1);
    constexpr std::string_view desktop = ".desktop";
    if (name.size() >= desktop.size() &&
        name.compare(name.size() - desktop.size(), desktop.size(), desktop) == 0)
        name.resize(name.size() - desktop.size());
    return name;
}

inline bool isVivaldiBrowser(const std::string &program) {
    const auto name = browserProgramName(program);
    return name == "vivaldi" || name == "vivaldi-stable" ||
           name == "vivaldi-snapshot" || name == "com.vivaldi.vivaldi";
}

inline bool isFirefoxBrowser(const std::string &program) {
    const auto name = browserProgramName(program);
    static constexpr std::string_view names[] = {
        "firefox", "firefox-bin", "firefox-esr", "firefox-developer-edition",
        "firefox-nightly", "org.mozilla.firefox", "org.mozilla.firefoxdeveloperedition",
        "librewolf", "io.gitlab.librewolf-community", "floorp", "one.ablaze.floorp",
        "zen", "zen-bin", "zen-browser", "app.zen_browser.zen",
        "waterfox", "net.waterfox.waterfox", "icecat", "org.gnu.icecat",
        "firedragon", "mercury", "seamonkey"
    };
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

// Browser identity, not Chromium runtime identity. An Electron executable or
// app id containing "chrome"/"opera" must never enable omnibox editing.
inline bool isChromiumBrowser(const std::string &program) {
    const auto name = browserProgramName(program);
    static constexpr std::string_view names[] = {
        "chrome", "google-chrome", "google-chrome-stable", "google-chrome-beta",
        "google-chrome-unstable", "chromium", "chromium-browser",
        "brave", "brave-browser", "brave-browser-beta", "brave-browser-nightly",
        "vivaldi", "vivaldi-stable", "vivaldi-snapshot",
        "msedge", "msedge-beta", "msedge-dev", "msedge-canary",
        "microsoft-edge", "microsoft-edge-stable", "microsoft-edge-beta",
        "microsoft-edge-dev", "opera", "opera-beta", "opera-developer",
        "com.google.chrome", "com.google.chrome.beta", "com.google.chrome.dev",
        "org.chromium.chromium", "com.brave.browser", "com.brave.browser.beta",
        "com.brave.browser.nightly", "com.vivaldi.vivaldi",
        "com.microsoft.edge", "com.opera.opera"
    };
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}
} // namespace skey
#endif
