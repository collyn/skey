#ifndef SKEY_SETTINGS_CONFIG_IO_H
#define SKEY_SETTINGS_CONFIG_IO_H

#include <string>
#include <vector>

class QString;

/// Resolved from $XDG_CONFIG_HOME/fcitx5/conf/ (fallback ~/.config/fcitx5/conf/)
std::string configDir();

/// Individual config file paths (exposed for backup/restore)
std::string skeyConfPath();
std::string appModesPath();
std::string macroPath();
std::string fcitx5ConfigPath();

/// Update channel for the in-GUI updater (persisted as "Stable" / "Dev").
enum class UpdateChannel { Stable = 0, Dev = 1 };

/// Main Skey configuration (maps to skey.conf)
struct SKeyConfig {
    std::string inputMethod  = "Telex";       // "Telex", "VNI"
    std::string outputMode   = "Auto";        // "Auto", "Uinput", "Surrounding Text", "Preedit"
    std::string charset      = "Unicode";     // "Unicode", "TCVN3 (ABC)", "VNI Windows"
    bool shortW       = false;   // Telex: bare 'w' → 'ư'
    bool bracketUO    = false;   // Telex: '[' → 'ơ', ']' → 'ư'
    bool freeMarking  = false;
    bool autoRestore  = true;
    bool dict         = false;  // auto-restore checks the word list, not rules
    bool showPreedit  = true;
    std::string chromiumAddressBarMode = "Auto";  // "Auto", "Uinput", "Surrounding Text", "Preedit", "No Vietnamese"
    bool debug        = false;
    bool enableMacro         = true;
    bool capitalizeMacro     = true;
    bool macroInOffMode      = false;
    std::string modeMenuKey  = "grave";
    std::string iconTheme    = "default";  // preset name ("default"/"v-blue"/"v-dark")
                                           // or custom filename ("my-logo.png")
    std::string customIconPath = "";       // kept for backward compat; not used by resolver
    UpdateChannel updateChannel = UpdateChannel::Stable;  // "Stable", "Dev"
};

/// Per-application mode overrides (maps to skey-app-modes.conf)
struct AppModesConfig {
    /// Ordered list of (programName, mode) pairs.
    /// mode values: "Auto", "Uinput", "Surrounding Text", "Preedit", "Excluded"
    std::vector<std::pair<std::string, std::string>> entries;
};

/// Macro entry (maps to skey-macro.conf)
struct MacroConfig {
    /// Ordered list of (shortcut, expansion) pairs.
    std::vector<std::pair<std::string, std::string>> entries;
};

// ── User dictionary (skey/user-dict.txt) ─────────────────────────────────
// One word per line; loaded by the addon and merged into the built-in
// Vietnamese dictionary when the "Dùng từ điển" option is enabled.
std::string userDictPath();
std::vector<std::string> readUserDict();
bool writeUserDict(const std::vector<std::string> &words);

// ── Read helpers ────────────────────────────────────────────────────────
SKeyConfig      readSkeyConfig();
AppModesConfig  readAppModesConfig();
MacroConfig     readMacroConfig();
std::string     readTriggerKey();     // from [Hotkey/TriggerKeys] in fcitx5 config

// ── Write helpers ───────────────────────────────────────────────────────
bool writeSkeyConfig(const SKeyConfig &cfg);
bool writeAppModesConfig(const AppModesConfig &cfg);
bool writeMacroConfig(const MacroConfig &cfg);
bool writeTriggerKey(const std::string &fcitx5Key);  // write to [Hotkey/TriggerKeys]/0

// ── Conversion helpers ──────────────────────────────────────────────────
/// Convert fcitx5 key format ("Control+space") → QKeySequence format ("Ctrl+Space")
std::string fcitx5KeyToQKeySeq(const std::string &fcitx5Key);
/// Convert QKeySequence format ("Ctrl+Space") → fcitx5 key format ("Control+space")
std::string qKeySeqToFcitx5(const std::string &qKeySeq);

// ── Defaults ────────────────────────────────────────────────────────────
SKeyConfig defaultConfig();

// ── Reload / Restart ──────────────────────────────────────────────────────
/// Runs fcitx5-remote -r to tell fcitx5 to reload its config.
/// Returns true if the command was launched successfully.
bool reloadFcitx5();

/// Hard-restart fcitx5 (-r -d) and reconnect Wayland compositor.
/// Use after package update when the .so binary has changed.
/// Returns true if the restart was attempted.
bool restartFcitx5();

// ── Icon resolution / import ─────────────────────────────────────────────
/// "$XDG_DATA_HOME/fcitx5" (fallback "~/.local/share/fcitx5")
std::string userDataDir();
/// "$XDG_DATA_HOME/fcitx5/skey/icons" (fallback "~/.local/share/fcitx5/skey/icons")
std::string userIconDir();

/// Scan userIconDir() for custom icon files (*.png, *.svg).
/// Returns a list of filenames (with extensions).
std::vector<std::string> listCustomIcons();

/// Copy an uploaded image into userIconDir() with the given filename.
/// Overwrites if file already exists. Returns the stored filename, or "" on failure.
std::string importCustomIcon(const QString &srcPath, const QString &desiredName);

/// Delete a custom icon file from userIconDir().
bool removeCustomIcon(const std::string &filename);

/// Resolve the effective icon path for a config (Qt flavour: PNG-first dirs).
std::string effectiveIconPath(const SKeyConfig &cfg);

#endif // SKEY_SETTINGS_CONFIG_IO_H
