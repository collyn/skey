#include "config_io.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QThread>

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
        else if (key == "Charset")      cfg.charset       = val;
        else if (key == "ShortW")       cfg.shortW        = parseBool(val);
        else if (key == "BracketUO")    cfg.bracketUO     = parseBool(val);
        else if (key == "FreeMarking")  cfg.freeMarking   = parseBool(val);
        else if (key == "AutoRestore")  cfg.autoRestore   = parseBool(val);
        else if (key == "ShowPreedit")  cfg.showPreedit   = parseBool(val);
        else if (key == "ChromiumAddressBarMode") cfg.chromiumAddressBarMode = val;
        else if (key == "Debug")        cfg.debug         = parseBool(val);
    }

    // Migration: the old "Telex W" input method is now Telex + ShortW.
    if (cfg.inputMethod == "Telex W" || cfg.inputMethod == "TelexW") {
        cfg.inputMethod = "Telex";
        cfg.shortW = true;
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
    out << "# Character set (Unicode / TCVN3 (ABC) / VNI Windows)" << "\n";
    out << "Charset="       << maybeQuote(cfg.charset)      << "\n";
    out << "# Telex: type w as ư"           << "\n";
    out << "ShortW="        << boolStr(cfg.shortW)          << "\n";
    out << "# Telex: type ][ as ư ơ"        << "\n";
    out << "BracketUO="     << boolStr(cfg.bracketUO)       << "\n";
    out << "# Free marking"                << "\n";
    out << "FreeMarking="   << boolStr(cfg.freeMarking)     << "\n";
    out << "# Auto restore non-Vietnamese" << "\n";
    out << "AutoRestore="   << boolStr(cfg.autoRestore)     << "\n";
    out << "# Show preedit"                << "\n";
    out << "ShowPreedit="   << boolStr(cfg.showPreedit)     << "\n";
    out << "# Chromium address bar mode (Uinput / Surrounding Text / Preedit / No Vietnamese)" << "\n";
    out << "ChromiumAddressBarMode=" << maybeQuote(cfg.chromiumAddressBarMode) << "\n";
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

        if (!val.empty() && val != "Excluded") {
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

// ── Environment fix ─────────────────────────────────────────────────────

/// Write a block of env exports to a file, ensuring it exists and
/// contains the correct values.  Works both for env-file style (key=value)
/// and shell-rc style (export KEY=VALUE).
/// If the block marker "# fcitx5-skey" is found, replace everything between
/// the marker lines.  Otherwise append the block at the end.
static void writeEnvBlock(const char *path, const char *block) {
    std::string content;
    {
        std::ifstream in(path);
        if (in.is_open()) {
            std::stringstream ss;
            ss << in.rdbuf();
            content = ss.str();
        }
    }

    const char *marker = "# fcitx5-skey";
    auto markerPos = content.find(marker);
    if (markerPos != std::string::npos) {
        // Replace existing block: from marker to end of block
        auto endMarker = content.find(marker, markerPos + 1);
        if (endMarker != std::string::npos) {
            content.replace(markerPos, endMarker - markerPos, block);
        } else {
            // Only one marker — replace from marker to end of file
            content.replace(markerPos, content.size() - markerPos, block);
        }
    } else {
        // No existing block — append
        if (!content.empty() && content.back() != '\n') {
            content += '\n';
        }
        content += block;
    }

    // Ensure parent directory exists
    std::string pathStr(path);
    auto slash = pathStr.rfind('/');
    if (slash != std::string::npos) {
        QDir().mkpath(QString::fromStdString(pathStr.substr(0, slash)));
    }

    std::ofstream out(path);
    if (out.is_open()) {
        out << content;
    }
}

static const char kEnvBlock[] =
    "# fcitx5-skey: required for IM to work in all apps (esp. AppImages)\n"
    "export GTK_IM_MODULE=fcitx\n"
    "export QT_IM_MODULE=ibus\n"
    "export XMODIFIERS=@im=fcitx\n"
    "export SDL_IM_MODULE=fcitx\n"
    "export GLFW_IM_MODULE=ibus\n"
    "# fcitx5-skey\n";

static void fixEnvironmentFiles() {
    const char *home = getenv("HOME");
    if (!home) return;

    std::string homeStr(home);

    // Files that need the env block
    writeEnvBlock((homeStr + "/.xprofile").c_str(), kEnvBlock);

    // KDE Plasma startup scripts (sourced at login)
    writeEnvBlock((homeStr + "/.config/plasma-workspace/env/skey.sh").c_str(), kEnvBlock);

    // Shell rc — ensures new terminals pick up correct env immediately
    writeEnvBlock((homeStr + "/.zshenv").c_str(), kEnvBlock);
    writeEnvBlock((homeStr + "/.profile").c_str(), kEnvBlock);

    // systemd environment.d (read by environment-d-generator at login)
    // NB: environment.d uses KEY=VALUE format without 'export'
    writeEnvBlock((homeStr + "/.config/environment.d/fcitx5-skey.conf").c_str(),
                  "# fcitx5-skey\n"
                  "GTK_IM_MODULE=fcitx\n"
                  "QT_IM_MODULE=ibus\n"
                  "XMODIFIERS=@im=fcitx\n"
                  "SDL_IM_MODULE=fcitx\n"
                  "GLFW_IM_MODULE=ibus\n");

    // Also update the running session environment (not just files).
    // systemctl set-environment: propagates to subsequently launched
    //   processes within the user slice (KDE launcher, etc.)
    // dbus-update-activation-environment: syncs D-Bus session bus
    //   so D-Bus activated services see the new vars
    QProcess::startDetached("systemctl",
        {"--user", "set-environment",
         "GTK_IM_MODULE=fcitx",
         "QT_IM_MODULE=ibus",
         "XMODIFIERS=@im=fcitx",
         "SDL_IM_MODULE=fcitx",
         "GLFW_IM_MODULE=ibus"});
    QProcess::startDetached("dbus-update-activation-environment",
        {"--systemd",
         "GTK_IM_MODULE", "QT_IM_MODULE", "XMODIFIERS",
         "SDL_IM_MODULE", "GLFW_IM_MODULE"});

    // Also update IM vars for the current process' environment
    // so child processes launched by the settings app inherit the fix
    setenv("GTK_IM_MODULE", "fcitx", 1);
    setenv("QT_IM_MODULE", "ibus", 1);
    setenv("XMODIFIERS", "@im=fcitx", 1);
    setenv("SDL_IM_MODULE", "fcitx", 1);
    setenv("GLFW_IM_MODULE", "ibus", 1);

}

// ── Reload / Restart fcitx5 ─────────────────────────────────────────────

bool reloadFcitx5() {
    return QProcess::startDetached("fcitx5-remote", {"-r"});
}

bool restartFcitx5() {
    // 0. Fix IM environment files so AppImages (Viber, etc.) can connect.
    //    Uses IBus protocol because fcitx5 exports org.freedesktop.IBus on D-Bus,
    //    and IBus plugin is bundled in Qt AppImages.
    fixEnvironmentFiles();

    // Hard-restart: kill and re-launch fcitx5, then reconnect Wayland
    // compositor so virtual keyboard doesn't silently fall back to "None".
    QProcess proc;

    // 1. Restart the daemon
    proc.start("fcitx5", {"-r", "-d"});
    if (!proc.waitForFinished(5000)) {
        // didn't finish in time — may still be OK
        proc.terminate();
    }

    // 2. Give fcitx5 time to start and accept D-Bus connections
    QThread::sleep(1);

    // 3. Wayland: re-trigger compositor virtual keyboard binding
    //    When fcitx5 restarts, the compositor does NOT automatically
    //    reconnect — we must re-apply the InputMethod preference.
    auto env = QProcessEnvironment::systemEnvironment();
    if (env.value("XDG_SESSION_TYPE") == "wayland") {
        QString desktop = env.value("XDG_CURRENT_DESKTOP");
        if (desktop == "KDE" || desktop == "kde" ||
            desktop == "KDE-Plasma" || desktop == "plasma") {
            // KWin: toggle InputMethod to force compositor to tear
            // down and re-establish the zwp_input_method_v2 connection.
            // Writing the same value won't trigger a reconnect — we
            // must briefly clear it so KWin sees a real change.
            const char *kwrite = "kwriteconfig6";
            QProcess which;
            which.start("which", {kwrite});
            if (which.waitForFinished(2000) && which.exitCode() != 0) {
                kwrite = "kwriteconfig5";  // fallback for Plasma 5
            }

            // Helper: call a KWin D-Bus method (Q_NOREPLY, fire-and-forget).
            // Tries reconfigure first, then reloadConfig for older Plasma.
            auto callKWin = [](const char *method) {
                QDBusMessage msg = QDBusMessage::createMethodCall(
                    QStringLiteral("org.kde.KWin"),
                    QStringLiteral("/KWin"),
                    QStringLiteral("org.kde.KWin"),
                    QString::fromUtf8(method));
                QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
            };

            // 1. Clear InputMethod → KWin drops the IM connection
            QProcess kw1;
            kw1.start(kwrite,
                {"--file", "kwinrc",
                 "--group", "Wayland",
                 "--key", "InputMethod", ""});
            kw1.waitForFinished(3000);

            // 2. Tell KWin to reload (now with empty IM)
            callKWin("reconfigure");
            callKWin("reloadConfig");
            QThread::msleep(600);

            // 3. Restore fcitx5 Wayland launcher → KWin reconnects
            QProcess kw2;
            kw2.start(kwrite,
                {"--file", "kwinrc",
                 "--group", "Wayland",
                 "--key", "InputMethod",
                 "/usr/share/applications/fcitx5-wayland-launcher.desktop"});
            kw2.waitForFinished(3000);

            // 4. Tell KWin again to pick up the restored IM
            callKWin("reconfigure");
            callKWin("reloadConfig");
        } else if (desktop == "GNOME" || desktop == "gnome" ||
                   desktop == "GNOME-Classic") {
            // GNOME Shell: toggle input sources to force reconnect
            QProcess gs;
            gs.start("gsettings", {"get",
                "org.gnome.desktop.input-sources", "sources"});
            if (gs.waitForFinished(3000)) {
                QString sources = QString::fromUtf8(gs.readAllStandardOutput()).trimmed();
                if (!sources.isEmpty() && sources != "@as []") {
                    QProcess::startDetached("gsettings",
                        {"set", "org.gnome.desktop.input-sources",
                         "sources", "[]"});
                    QThread::msleep(500);
                    QProcess::startDetached("gsettings",
                        {"set", "org.gnome.desktop.input-sources",
                         "sources", sources});
                }
            }
        }
    }

    return true;
}
