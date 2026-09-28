#ifndef SKEY_BROWSER_IDENTITY_H
#define SKEY_BROWSER_IDENTITY_H

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>

namespace skey {
// Browser identity, not Chromium runtime identity. An Electron executable or
// app id containing "chrome"/"opera" must never enable omnibox editing.
inline bool isChromiumBrowser(const std::string &program) {
    std::string name = program;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name.erase(0, slash + 1);
    constexpr std::string_view desktop = ".desktop";
    if (name.size() >= desktop.size() &&
        name.compare(name.size() - desktop.size(), desktop.size(), desktop) == 0)
        name.resize(name.size() - desktop.size());
    static constexpr std::string_view names[] = {
        "chrome", "google-chrome", "google-chrome-stable", "google-chrome-beta",
        "google-chrome-unstable", "chromium", "chromium-browser",
        "brave", "brave-browser", "brave-browser-beta", "brave-browser-nightly",
        "vivaldi", "vivaldi-stable", "vivaldi-snapshot",
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
