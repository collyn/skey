#ifndef SKEY_ICON_RESOLVER_H
#define SKEY_ICON_RESOLVER_H

#include <string>
#include <vector>

namespace skey {

// Known preset icon theme names.
inline constexpr const char *kIconThemeDefault = "default";
inline constexpr const char *kIconThemeVBlue   = "v-blue";
inline constexpr const char *kIconThemeVDark   = "v-dark";
inline constexpr const char *kIconThemeVLight  = "v-light";
inline constexpr const char *kIconThemeVRed    = "v-red";
inline constexpr const char *kIconThemeVnFlag  = "vn-flag";

// Returns true if the theme is one of the built-in presets.
inline bool isPresetTheme(const std::string &theme) {
    return theme == kIconThemeDefault ||
           theme == kIconThemeVBlue ||
           theme == kIconThemeVDark ||
           theme == kIconThemeVLight ||
           theme == kIconThemeVRed ||
           theme == kIconThemeVnFlag;
}

// Returns the base file name (no extension) of the system-installed preset
// icon. Unknown/empty themes map to "fcitx-skey" (the default icon).
const char *presetIconBaseName(const std::string &theme);

struct IconSearchPaths {
    // User data directory, e.g. "/home/u/.local/share/fcitx5"
    // (trailing slash is OK).
    std::string userDataDir;
    // System install directories, searched in order. Each is probed as
    // "<dir>/<base>.svg" then "<dir>/<base>.png".
    std::vector<std::string> systemDirs;
    // Absolute fallback path — the compile-time default icon, always present.
    std::string fallback;
};

// Resolve the effective icon path. Resolution order:
//   1. If theme is a known preset: probe <systemDir>/<base>.{svg,png}
//   2. Otherwise treat theme as a custom filename: probe
//      <userDataDir>/skey/icons/<theme> directly (theme is already a filename)
//   3. For backward-compat "custom" theme: probe custom.{svg,png} in user dir
//   4. fallback
std::string resolveIconPath(const std::string &iconTheme,
                            const IconSearchPaths &paths);

} // namespace skey

#endif // SKEY_ICON_RESOLVER_H
