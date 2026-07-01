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

// ── Defaults ────────────────────────────────────────────────────────────

SKeyConfig defaultConfig() {
    return SKeyConfig{};   // struct initializers are the defaults
}

// ── Reload fcitx5 ───────────────────────────────────────────────────────

bool reloadFcitx5() {
    return QProcess::startDetached("fcitx5-remote", {"-r"});
}
