#include "config_io.h"

#include <fstream>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QThread>

#include "../icon_resolver.h"


// ── Path resolution ────────────────────────────────────────────────────

std::string configDir() {
    QString cfgHome = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    // On Linux this is ~/.config
    QString fcitxConf = cfgHome + "/fcitx5/conf";
    return fcitxConf.toStdString();
}

std::string skeyConfPath() { return configDir() + "/skey.conf"; }
std::string appModesPath() { return configDir() + "/skey-app-modes.conf"; }
std::string macroPath() { return configDir() + "/skey-macro.conf"; }

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
        else if (key == "Charset")      cfg.charset       = val;
        else if (key == "ShortW")       cfg.shortW        = parseBool(val);
        else if (key == "BracketUO")    cfg.bracketUO     = parseBool(val);
        else if (key == "FreeMarking")  cfg.freeMarking   = parseBool(val);
        else if (key == "AutoRestore")  cfg.autoRestore   = parseBool(val);
        else if (key == "Dict")         cfg.dict          = parseBool(val);
        else if (key == "ShowPreedit")  cfg.showPreedit   = parseBool(val);
        else if (key == "ChromiumAddressBarMode") cfg.chromiumAddressBarMode = val;
        else if (key == "Debug")        cfg.debug         = parseBool(val);
        else if (key == "EnableMacro")   cfg.enableMacro    = parseBool(val);
        else if (key == "CapitalizeMacro") cfg.capitalizeMacro = parseBool(val);
        else if (key == "MacroInOffMode")  cfg.macroInOffMode  = parseBool(val);
        else if (key == "ModeMenuKey")   cfg.modeMenuKey    = val;
        else if (key == "IconTheme")       cfg.iconTheme      = val;
        else if (key == "CustomIconPath")  cfg.customIconPath = val;
        else if (key == "UpdateChannel")
            cfg.updateChannel = (val == "Dev") ? UpdateChannel::Dev
                                               : UpdateChannel::Stable;
        else if (key == "UILanguage")
            cfg.uiLanguage = (val == "en") ? "en" : "vi";
    }

    // Migration: the old "Telex W" input method is now Telex + ShortW.
    if (cfg.inputMethod == "Telex W" || cfg.inputMethod == "TelexW") {
        cfg.inputMethod = "Telex";
        cfg.shortW = true;
    }
    return cfg;
}

bool writeSkeyConfig(const SKeyConfig &cfg) {
    // ~/.config/fcitx5/conf may not exist yet (fcitx5 only creates it when
    // it saves an addon config — on NixOS the system config lives in
    // /etc/xdg/fcitx5 and the user dir can stay empty).
    if (!QDir().mkpath(QString::fromStdString(configDir()))) return false;
    std::ofstream out(skeyConfPath());
    if (!out.is_open()) return false;

    out << "# Input Method"                 << "\n";
    out << "InputMethod="   << maybeQuote(cfg.inputMethod)  << "\n";
    out << "# Output Mode"                  << "\n";
    out << "OutputMode="    << maybeQuote(cfg.outputMode)   << "\n";
    out << "# Character set (Unicode / TCVN3 (ABC) / VNI Windows / Windows CP1258 / VIQR)" << "\n";
    out << "Charset="       << maybeQuote(cfg.charset)      << "\n";
    out << "# Telex: type w as ư"           << "\n";
    out << "ShortW="        << boolStr(cfg.shortW)          << "\n";
    out << "# Telex: type ][ as ư ơ"        << "\n";
    out << "BracketUO="     << boolStr(cfg.bracketUO)       << "\n";
    out << "# Free marking"                << "\n";
    out << "FreeMarking="   << boolStr(cfg.freeMarking)     << "\n";
    out << "# Auto restore non-Vietnamese" << "\n";
    out << "AutoRestore="   << boolStr(cfg.autoRestore)     << "\n";
    out << "# Dictionary mode (restore checks real words)" << "\n";
    out << "Dict="          << boolStr(cfg.dict)            << "\n";
    out << "# Show preedit"                << "\n";
    out << "ShowPreedit="   << boolStr(cfg.showPreedit)     << "\n";
    out << "# Chromium address bar mode (Uinput / Surrounding Text / Preedit / No Vietnamese)" << "\n";
    out << "ChromiumAddressBarMode=" << maybeQuote(cfg.chromiumAddressBarMode) << "\n";
    out << "# Enable debug logging"        << "\n";
    out << "Debug="         << boolStr(cfg.debug)           << "\n";
    out << "# Macro / Gõ tắt"                << "\n";
    out << "EnableMacro="    << boolStr(cfg.enableMacro)     << "\n";
    out << "CapitalizeMacro=" << boolStr(cfg.capitalizeMacro) << "\n";
    out << "MacroInOffMode=" << boolStr(cfg.macroInOffMode)  << "\n";
    out << "ModeMenuKey="   << maybeQuote(cfg.modeMenuKey)   << "\n";
    out << "# Icon theme (default / v-blue / v-dark / v-light / v-red / vn-flag / custom)" << "\n";
    out << "IconTheme="      << maybeQuote(cfg.iconTheme)      << "\n";
    out << "# Custom icon path (only when IconTheme=custom)"    << "\n";
    out << "CustomIconPath=" << maybeQuote(cfg.customIconPath)  << "\n";
    out << "# Update channel (Stable / Dev)"    << "\n";
    out << "UpdateChannel=" << (cfg.updateChannel == UpdateChannel::Dev ? "Dev" : "Stable") << "\n";
    out << "# Settings GUI language (vi / en)" << "\n";
    out << "UILanguage=" << cfg.uiLanguage << "\n";
    out << "MacroEditor=fcitx://config/addon/skey/skey-macro" << "\n";

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

        if (!val.empty() && val != "Excluded") {
            // Migrate legacy config
            if (val == "SurroundingTextSlow" || val == "SurroundingText")
                val = "Surrounding Text";
            cfg.entries.emplace_back(name, val);
        }
    }
    return cfg;
}

bool writeAppModesConfig(const AppModesConfig &cfg) {
    if (!QDir().mkpath(QString::fromStdString(configDir()))) return false;
    std::ofstream out(appModesPath());
    if (!out.is_open()) return false;

    for (auto &[name, mode] : cfg.entries) {
        out << name << "=" << mode << "\n";
    }
    return out.good();
}

// ── skey-macro.conf read/write ──────────────────────────────────────────

MacroConfig readMacroConfig() {
    MacroConfig cfg;
    std::ifstream in(macroPath());
    if (!in.is_open()) return cfg;

    // Fcitx5 format: [Entries/N]\nKey=...\nValue=...
    // Legacy flat format: key=value (one per line, fallback)
    std::string line;
    std::string currentKey, currentValue;
    bool inEntry = false;

    while (std::getline(in, line)) {
        rtrim(line);
        if (line.empty() || line[0] == '#') continue;

        // Fcitx5 section: [Entries/N]
        if (line.size() > 9 && line.compare(0, 9, "[Entries/") == 0) {
            // Flush previous entry if any
            if (!currentKey.empty()) {
                cfg.entries.emplace_back(currentKey, currentValue);
                currentKey.clear(); currentValue.clear();
            }
            inEntry = true;
            continue;
        }
        // Other sections end the entries block
        if (line[0] == '[') { inEntry = false; continue; }

        if (inEntry) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            while (!key.empty() && key.front() == ' ') key.erase(0, 1);
            rtrim(key);
            rtrim(val);
            stripQuotes(val);

            if (key == "Key") {
                currentKey = val;
            } else if (key == "Value") {
                currentValue = val;
            }
            continue;
        }

        // Legacy flat format (no [Entries] section found)
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && key.front() == ' ') key.erase(0, 1);
        rtrim(key);
        rtrim(val);
        stripQuotes(val);
        if (!key.empty() && !val.empty()) {
            cfg.entries.emplace_back(key, val);
        }
    }
    // Flush last entry
    if (!currentKey.empty()) {
        cfg.entries.emplace_back(currentKey, currentValue);
    }
    return cfg;
}

bool writeMacroConfig(const MacroConfig &cfg) {
    if (!QDir().mkpath(QString::fromStdString(configDir()))) return false;
    std::ofstream out(macroPath());
    if (!out.is_open()) return false;

    out << "# Macro / Gõ tắt definitions\n";
    out << "\n";
    for (size_t i = 0; i < cfg.entries.size(); ++i) {
        out << "[Entries/" << std::to_string(i) << "]\n";
        out << "Key=" << maybeQuote(cfg.entries[i].first) << "\n";
        out << "Value=" << maybeQuote(cfg.entries[i].second) << "\n";
    }
    return out.good();
}

// ── fcitx5 global config (trigger key) ─────────────────────────────────

std::string fcitx5ConfigPath() {
    return configDir().substr(0, configDir().rfind("/conf")) + "/config";
}

std::string readTriggerKey() {
    std::ifstream in(fcitx5ConfigPath());
    if (!in.is_open()) return "Control+space";

    std::string line;
    bool inTriggerSection = false;
    bool sawSection = false;
    while (std::getline(in, line)) {
        rtrim(line);
        if (line == "[Hotkey/TriggerKeys]") {
            inTriggerSection = true;
            sawSection = true;
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
    // Section exists but has no 0= key: the trigger key was unbound in the
    // GUI.  Distinguish from "no config at all" (fresh install → default).
    if (sawSection) return "";
    return "Control+space";
}

bool writeTriggerKey(const std::string &fcitx5Key) {
    // Unbinding is an empty value: drop the 0= line entirely instead of
    // writing "0=" — fcitx5 treats an empty [Hotkey/TriggerKeys] as "no
    // trigger key", and readTriggerKey() maps that state back to "".
    std::string path = fcitx5ConfigPath();
    std::ifstream in(path);
    if (!in.is_open()) {
        if (fcitx5Key.empty()) return true;  // nothing bound, nothing to write
        // No user-level fcitx5 config yet — NixOS keeps the system config
        // in /etc/xdg/fcitx5 (environment.etc), so ~/.config/fcitx5/config
        // often doesn't exist.  Create it with just the trigger key instead
        // of failing the whole save.
        QDir().mkpath(QFileInfo(QString::fromStdString(path)).absolutePath());
        std::ofstream out(path);
        if (!out.is_open()) return false;
        out << "[Hotkey/TriggerKeys]\n"
            << "0=" << fcitx5Key << "\n";
        return out.good();
    }

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
                if (!wrote && !fcitx5Key.empty()) {
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
                    if (!fcitx5Key.empty())
                        lines.push_back("0=" + fcitx5Key);
                    wrote = true;  // empty value: line removed (unbound)
                    continue;
                }
            }
            lines.push_back(line);
            continue;
        }
        lines.push_back(line);
    }

    // EOF, still inside the trigger section: append the key.
    if (inTriggerSection && !wrote && !fcitx5Key.empty()) {
        lines.push_back("0=" + fcitx5Key);
    }
    // EOF, no [Hotkey/TriggerKeys] section at all: append one.
    if (!inTriggerSection && !wrote && !fcitx5Key.empty()) {
        lines.push_back("");
        lines.push_back("[Hotkey/TriggerKeys]");
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

// ── Reload / Restart fcitx5 ─────────────────────────────────────────────

bool reloadFcitx5() {
    return QProcess::startDetached("fcitx5-remote", {"-r"});
}

bool restartFcitx5() {
    // Restart fcitx5 through the packaged helper script: it waits for the
    // new instance to finish loading (waylandim addon registers ~1s after
    // the main D-Bus name) and then toggles KWin's VirtualKeyboard so
    // zwp_input_method_v2 reconnects without apps losing their text-input
    // registration (see scripts/skey-restart-fcitx5).  Fallback: bare
    // restart — apps may need a focus toggle to reconnect.
    QString script = QStandardPaths::findExecutable("skey-restart-fcitx5");
    bool ok = script.isEmpty()
                  ? QProcess::startDetached("fcitx5", {"-r", "-d"})
                  : QProcess::startDetached(script, {});

    // Restart uinput server to pick up updated binary.
    QString user = qgetenv("USER");
    if (!user.isEmpty()) {
        QProcess::startDetached("systemctl", {"try-restart",
            QString("fcitx5-skey-uinput-server@%1.service").arg(user)});
    }

    return ok;
}

// ── Icon resolution / import ─────────────────────────────────────────────

std::string userDataDir() {
    QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data.isEmpty()) data = QDir::homePath() + "/.local/share";
    return (data + "/fcitx5").toStdString();
}

std::string userIconDir() {
    return userDataDir() + "/skey/icons";
}

// ── User dictionary ───────────────────────────────────────────────────────

std::string userDictPath() {
    return userDataDir() + "/skey/user-dict.txt";
}

std::vector<std::string> readUserDict() {
    std::vector<std::string> words;
    std::ifstream in(userDictPath());
    if (!in.is_open()) return words;
    std::string line;
    while (std::getline(in, line)) {
        rtrim(line);
        if (line.empty() || line[0] == '#') continue;
        words.push_back(line);
    }
    return words;
}

bool writeUserDict(const std::vector<std::string> &words) {
    QString dir = QString::fromStdString(userDataDir() + "/skey");
    if (!QDir().mkpath(dir)) return false;
    std::ofstream out(userDictPath());
    if (!out.is_open()) return false;

    out << "# Từ điển cá nhân — mỗi từ một dòng\n";
    out << "# Các từ này được thêm vào từ điển tiếng Việt khi bật\n";
    out << "# \"Dùng từ điển\" ở tab Chung.  Sửa xong nhấn Áp dụng.\n";
    out << "\n";
    for (const auto &w : words) {
        out << w << "\n";
    }
    return true;
}

std::vector<std::string> listCustomIcons() {
    std::vector<std::string> result;
    QDir dir(QString::fromStdString(userIconDir()));
    if (!dir.exists()) return result;
    QStringList filters;
    filters << "*.png" << "*.svg";
    for (const auto &fi : dir.entryInfoList(filters, QDir::Files, QDir::Name)) {
        result.push_back(fi.fileName().toStdString());
    }
    return result;
}

std::string importCustomIcon(const QString &srcPath, const QString &desiredName) {
    QFileInfo info(srcPath);
    QString ext = info.suffix().toLower();
    if (ext != "svg" && ext != "png") return {};

    QDir dir(QString::fromStdString(userIconDir()));
    if (!dir.exists() && !dir.mkpath(".")) return {};

    // Use desired base name, keeping the source extension
    QString base = desiredName.isEmpty()
        ? info.completeBaseName()
        : QFileInfo(desiredName).completeBaseName();
    QString dest = dir.filePath(base + "." + ext);

    // Remove stale file with the other extension under the same base name
    QString other = dir.filePath(base + "." + (ext == "png" ? "svg" : "png"));
    QFile::remove(other);

    if (ext == "svg") {
        // SVG: vector, copy as-is
        if (QFile::exists(dest)) QFile::remove(dest);
        if (!QFile::copy(srcPath, dest)) return {};
    } else {
        // PNG: resize to max 128×128 (tray icons are tiny, don't waste space)
        QImage img(srcPath);
        if (img.isNull()) return {};
        if (img.width() > 128 || img.height() > 128) {
            img = img.scaled(128, 128, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        }
        if (QFile::exists(dest)) QFile::remove(dest);
        if (!img.save(dest, "PNG")) return {};
    }

    return (base + "." + ext).toStdString();  // return just the filename
}

bool removeCustomIcon(const std::string &filename) {
    QString path = QString::fromStdString(userIconDir()) + "/"
                 + QString::fromStdString(filename);
    return QFile::remove(path);
}

std::string effectiveIconPath(const SKeyConfig &cfg) {
    skey::IconSearchPaths paths;
    paths.userDataDir = userDataDir();
    // SVG first: renders crisp at any scale/DPR, while the 128px PNG gets
    // visibly soft or pixelated at fractional HiDPI scaling.  Both callers
    // (info tab + settings window icon) fall back to the PNG when QIcon
    // returns null — i.e. exactly when the QtSvg plugin is missing — so
    // environments without libqt6svg stay covered.
    paths.systemDirs = {
        // Packaged install dir first — distros without /usr/share (NixOS)
        // resolve the presets from the package itself.
        FCITX_SKEY_ICON_DIR "/hicolor/scalable/apps",
        FCITX_SKEY_ICON_DIR "/hicolor/128x128/apps",
        "/usr/share/icons/hicolor/scalable/apps",
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/pixmaps",
    };
    paths.fallback = FCITX_SKEY_ICON_PATH; // compile-time default
    return skey::resolveIconPath(cfg.iconTheme, paths);
}
