#include "config_io.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <QProcess>
#include <QStandardPaths>
#include <QString>

// ── Path resolution ────────────────────────────────────────────────────

std::string configDir() {
    QString cfgHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    // On Linux this is ~/.config
    QString fcitxConf = cfgHome + "/fcitx5/conf";
    return fcitxConf.toStdString();
}

static std::string skeyConfPath() { return configDir() + "/skey.conf"; }
static std::string appModesPath() { return configDir() + "/skey-app-modes.conf"; }

// ── Helpers ─────────────────────────────────────────────────────────────

/// Strip surrounding double-quotes from a string (in-place).
static void stripQuotes(std::string &s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
}

/// Quote a value if it contains spaces or Unicode characters.
static std::string maybeQuote(const std::string &val) {
    // If value is empty or contains space / higher-unicode chars, quote it
    if (val.empty()) return "\"\"";
    for (char c : val) {
        if (c == ' ' || static_cast<unsigned char>(c) > 127) {
            return '"' + val + '"';
        }
    }
    return val;
}

/// Parse a fcitx5 bool string ("True"/"False").
static bool parseBool(const std::string &s) {
    return s == "True" || s == "true";
}

static std::string boolStr(bool v) { return v ? "True" : "False"; }

/// Trim trailing whitespace (including \r \n) in-place.
static void rtrim(std::string &s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
}

// ── skey.conf read/write ────────────────────────────────────────────────

SKeyConfig readSkeyConfig() {
    SKeyConfig cfg;
    std::ifstream in(skeyConfPath());
    if (!in.is_open()) return cfg; // use defaults

    std::string line;
    while (std::getline(in, line)) {
        rtrim(line);
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim leading spaces from key
        while (!key.empty() && key.front() == ' ') key.erase(0, 1);
        rtrim(key);

        stripQuotes(val);

        if (key == "InputMethod")      cfg.inputMethod  = val;
        else if (key == "OutputMode")   cfg.outputMode   = val;
        else if (key == "TonePosition") cfg.tonePosition = val;
        else if (key == "FreeMarking")  cfg.freeMarking   = parseBool(val);
        else if (key == "AutoRestore")  cfg.autoRestore   = parseBool(val);
        else if (key == "ShowPreedit")  cfg.showPreedit   = parseBool(val);
        else if (key == "ChromiumAddressBarPreedit") cfg.chromiumAddressBarPreedit = parseBool(val);
        else if (key == "Debug")        cfg.debug         = parseBool(val);
    }
    return cfg;
}

bool writeSkeyConfig(const SKeyConfig &cfg) {
    std::ofstream out(skeyConfPath());
    if (!out.is_open()) return false;

    out << "# Input Method"                 << "\n";
    out << "InputMethod="   << maybeQuote(cfg.inputMethod)  << "\n";
    out << "# Output Mode"                  << "\n";
    out << "OutputMode="    << maybeQuote(cfg.outputMode)   << "\n";
    out << "# Tone Mark Position"          << "\n";
    out << "TonePosition="  << maybeQuote(cfg.tonePosition) << "\n";
    out << "# Free marking"                << "\n";
    out << "FreeMarking="   << boolStr(cfg.freeMarking)     << "\n";
    out << "# Auto restore non-Vietnamese" << "\n";
    out << "AutoRestore="   << boolStr(cfg.autoRestore)     << "\n";
    out << "# Show preedit"                << "\n";
    out << "ShowPreedit="   << boolStr(cfg.showPreedit)     << "\n";
    out << "# Auto Preedit for address bar" << "\n";
    out << "ChromiumAddressBarPreedit=" << boolStr(cfg.chromiumAddressBarPreedit) << "\n";
    out << "# Enable debug logging"        << "\n";
    out << "Debug="         << boolStr(cfg.debug)           << "\n";

    return out.good();
}

// ── skey-app-modes.conf read/write ──────────────────────────────────────

AppModesConfig readAppModesConfig() {
    AppModesConfig cfg;
    std::ifstream in(appModesPath());
    if (!in.is_open()) return cfg;

    std::string line;
    while (std::getline(in, line)) {
        rtrim(line);
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string name = line.substr(0, eq);
        std::string val  = line.substr(eq + 1);

        while (!name.empty() && name.front() == ' ') name.erase(0, 1);
        rtrim(name);
        rtrim(val);

        if (!name.empty() && !val.empty() && val != "Excluded") {
            // Migrate legacy config
            if (val == "SurroundingTextSlow") val = "SurroundingText";
            cfg.entries.emplace_back(name, val);
        }
    }
    return cfg;
}

bool writeAppModesConfig(const AppModesConfig &cfg) {
    std::ofstream out(appModesPath());
    if (!out.is_open()) return false;

    for (auto &[name, mode] : cfg.entries) {
        out << name << "=" << mode << "\n";
    }
    return out.good();
}

// ── fcitx5 global config (trigger key) ─────────────────────────────────

static std::string fcitx5ConfigPath() {
    return configDir().substr(0, configDir().rfind("/conf")) + "/config";
}

std::string readTriggerKey() {
    std::ifstream in(fcitx5ConfigPath());
    if (!in.is_open()) return "Control+space";

    std::string line;
    bool inTriggerSection = false;
    while (std::getline(in, line)) {
        rtrim(line);
        if (line == "[Hotkey/TriggerKeys]") {
            inTriggerSection = true;
            continue;
        }
        if (inTriggerSection) {
            if (line.empty()) continue;
            if (line[0] == '[') break;  // next section
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                rtrim(key);
                rtrim(val);
                if (key == "0") return val;
            }
        }
    }
    return "Control+space";
}

bool writeTriggerKey(const std::string &fcitx5Key) {
    std::string path = fcitx5ConfigPath();
    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::vector<std::string> lines;
    std::string line;
    bool inTriggerSection = false;
    bool wrote = false;

    while (std::getline(in, line)) {
        std::string trimmed = line;
        rtrim(trimmed);

        if (trimmed == "[Hotkey/TriggerKeys]") {
            inTriggerSection = true;
            lines.push_back(line);
            continue;
        }
        if (inTriggerSection) {
            if (!trimmed.empty() && trimmed[0] == '[') {
                // Next section — insert 0= if not yet written
                if (!wrote) {
                    lines.push_back("0=" + fcitx5Key);
                    wrote = true;
                }
                inTriggerSection = false;
                lines.push_back(line);
                continue;
            }
            auto eq = trimmed.find('=');
            if (eq != std::string::npos) {
                std::string key = trimmed.substr(0, eq);
                rtrim(key);
                if (key == "0") {
                    lines.push_back("0=" + fcitx5Key);
                    wrote = true;
                    continue;
                }
            }
            lines.push_back(line);
            continue;
        }
        lines.push_back(line);
    }

    // If still in trigger section at EOF and not written
    if (inTriggerSection && !wrote) {
        lines.push_back("0=" + fcitx5Key);
    }

    in.close();

    std::ofstream out(path);
    if (!out.is_open()) return false;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << "\n";
    }
    return out.good();
}

// ── Key format conversion ──────────────────────────────────────────────

std::string fcitx5KeyToQKeySeq(const std::string &fcitx5Key) {
    std::string result = fcitx5Key;
    // "Control+" → "Ctrl+"
    for (size_t p = 0; (p = result.find("Control+", p)) != std::string::npos; p += 5)
        result.replace(p, 8, "Ctrl+");
    // "Super+" → "Meta+"
    for (size_t p = 0; (p = result.find("Super+", p)) != std::string::npos; p += 5)
        result.replace(p, 6, "Meta+");
    // Lowercase modifier names
    // (already done by the replaces above — "Ctrl", "Meta" are correct for QKeySequence)
    return result;
}

std::string qKeySeqToFcitx5(const std::string &qKeySeq) {
    std::string result = qKeySeq;
    // "Ctrl+" → "Control+"
    for (size_t p = 0; (p = result.find("Ctrl+", p)) != std::string::npos; p += 8)
        result.replace(p, 5, "Control+");
    // "Meta+" → "Super+"
    for (size_t p = 0; (p = result.find("Meta+", p)) != std::string::npos; p += 6)
        result.replace(p, 5, "Super+");
    return result;
}

// ── Defaults ────────────────────────────────────────────────────────────

SKeyConfig defaultConfig() {
    return SKeyConfig{};   // struct initializers are the defaults
}

// ── Reload fcitx5 ───────────────────────────────────────────────────────

bool reloadFcitx5() {
    return QProcess::startDetached("fcitx5-remote", {"-r"});
}
