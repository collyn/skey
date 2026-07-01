#ifndef SKEY_SETTINGS_CONFIG_IO_H
#define SKEY_SETTINGS_CONFIG_IO_H

#include <string>
#include <vector>

/// Resolved from $XDG_CONFIG_HOME/fcitx5/conf/ (fallback ~/.config/fcitx5/conf/)
std::string configDir();

/// Main Skey configuration (maps to skey.conf)
struct SKeyConfig {
    std::string inputMethod  = "Telex";       // "Telex", "VNI", "Telex W"
    std::string outputMode   = "Uinput";      // "Uinput", "Surrounding Text", "Preedit"
    std::string tonePosition = "Modern (hoà)"; // "Modern (hoà)", "Traditional (hòa)"
    bool freeMarking  = true;
    bool autoRestore  = true;
    bool showPreedit  = true;
    bool debug        = false;
};

/// Per-application mode overrides (maps to skey-app-modes.conf)
struct AppModesConfig {
    /// Ordered list of (programName, mode) pairs.
    /// mode values: "Uinput", "SurroundingText", "Preedit", "Excluded"
    std::vector<std::pair<std::string, std::string>> entries;
};

// ── Read helpers ────────────────────────────────────────────────────────
SKeyConfig      readSkeyConfig();
AppModesConfig  readAppModesConfig();

// ── Write helpers ───────────────────────────────────────────────────────
bool writeSkeyConfig(const SKeyConfig &cfg);
bool writeAppModesConfig(const AppModesConfig &cfg);

// ── Defaults ────────────────────────────────────────────────────────────
SKeyConfig defaultConfig();

// ── Reload ──────────────────────────────────────────────────────────────
/// Runs fcitx5-remote -r to tell fcitx5 to reload its config.
/// Returns true if the command was launched successfully.
bool reloadFcitx5();

#endif // SKEY_SETTINGS_CONFIG_IO_H
