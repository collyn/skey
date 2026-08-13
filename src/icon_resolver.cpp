#include "icon_resolver.h"
#include <unistd.h>

namespace skey {

// ── helpers ───────────────────────────────────────────────────────────────

static bool fileReadable(const std::string &path) {
    return !path.empty() && access(path.c_str(), R_OK) == 0;
}

static std::string joinPath(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

// ── presetIconBaseName ────────────────────────────────────────────────────

const char *presetIconBaseName(const std::string &theme) {
    if (theme == kIconThemeVBlue) return "fcitx-skey-v-blue";
    if (theme == kIconThemeVDark) return "fcitx-skey-v-dark";
    if (theme == kIconThemeVLight) return "fcitx-skey-v-light";
    if (theme == kIconThemeVRed) return "fcitx-skey-v-red";
    if (theme == kIconThemeVnFlag) return "fcitx-skey-vn-flag";
    // default and unknown themes map to the original icon
    return "fcitx-skey";
}

// ── resolveIconPath ───────────────────────────────────────────────────────

std::string resolveIconPath(const std::string &iconTheme,
                            const IconSearchPaths &paths) {
    const std::string theme = iconTheme.empty() ? kIconThemeDefault : iconTheme;

    // 1. Known preset theme — probe system directories.
    //    PNG first: Qt renders PNG natively without plugins; engines with
    //    SVG-only dirs still get SVG because PNG won't exist there.
    if (isPresetTheme(theme)) {
        const char *base = presetIconBaseName(theme);
        for (const auto &dir : paths.systemDirs) {
            for (const char *ext : {".png", ".svg"}) {
                std::string p = joinPath(dir, std::string(base) + ext);
                if (fileReadable(p)) return p;
            }
        }
    }

    // 2. Custom icon — theme is a filename living in the user icon directory
    if (!theme.empty()) {
        std::string iconDir = joinPath(joinPath(paths.userDataDir, "skey"), "icons");
        std::string customPath = joinPath(iconDir, theme);
        if (fileReadable(customPath)) return customPath;
    }

    // 3. Backward compat: "custom" theme → probe custom.{svg,png} in user dir
    if (theme == "custom") {
        std::string iconDir = joinPath(joinPath(paths.userDataDir, "skey"), "icons");
        for (const char *name : {"custom.svg", "custom.png"}) {
            std::string p = joinPath(iconDir, name);
            if (fileReadable(p)) return p;
        }
    }

    // 4. Fallback — the compile-time default.
    if (fileReadable(paths.fallback)) return paths.fallback;

    // 5. Last resort: probe system dirs for original icon in any format.
    //    Covers missing PNG after rpmbuild without rsvg-convert.
    for (const auto &dir : paths.systemDirs) {
        for (const char *ext : {".png", ".svg"}) {
            std::string p = joinPath(dir, std::string(presetIconBaseName(kIconThemeDefault)) + ext);
            if (fileReadable(p)) return p;
        }
    }

    // 6. Nothing found — return fallback path anyway (caller handles null QIcon).
    return paths.fallback;
}

} // namespace skey
