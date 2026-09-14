#ifndef SKEY_APP_DELAY_KEY_H
#define SKEY_APP_DELAY_KEY_H

// Shared by the addon (src/engine.cpp) and the settings GUI: the per-app key
// scheme and the value format of conf/skey-app-delay-overrides.conf must be
// identical on both sides, or an override written by the GUI would never be
// found by the engine.  Header-only and dependency-free on purpose (no Qt,
// no fcitx5) so a plain C++ unit test can cover it.

#include <cstdint>
#include <cstdlib>
#include <string>

namespace skey {

/// Manual per-app delay override, in milliseconds.  -1 = automatic (the
/// engine's adaptive computation).  A set field is applied verbatim,
/// whatever the AutoDelay option is.
struct AppDelayOverride {
    int paceMs = -1;      // gap between injected BackSpace keys
    int preCommitMs = -1; // wait after the BS, before commitText
    int postCommitMs = -1; // wait after commitText, before replay / next key

    bool any() const {
        return paceMs >= 0 || preCommitMs >= 0 || postCommitMs >= 0;
    }
};

// Bounds: a hand-edited or corrupt file must never freeze the IME
// (usleep with a bogus huge value blocks the whole event loop).
constexpr int kMaxPaceMs = 100;
constexpr int kMaxPreCommitMs = 500;
constexpr int kMaxPostCommitMs = 500;

/// Map a program name to a RawConfig/ini-safe per-app key (AutoDelay).
/// '/' is the RawConfig path separator (RawConfig::get splits on it) — an
/// app name containing it would silently create a nested section; '=',
/// '#', ';', quotes and newlines would corrupt the line shape.  The
/// '@w'/'@x' suffix keeps Wayland and X11 statistics apart: the two paths
/// have different delay tables, so mixing their round trips mis-sizes both.
inline std::string appDelayKey(const std::string &prog, bool wayland) {
    std::string k;
    k.reserve(prog.size() + 2);
    for (unsigned char c : prog) {
        if (c == '/' || c == '\\' || c == '=' || c == '#' || c == ';' ||
            c == '[' || c == ']' || c == '"' || c == '\n' || c == '\r' ||
            c == '\t' || c == '\0')
            k.push_back('_');
        else
            k.push_back(static_cast<char>(c));
    }
    size_t b = k.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return {}; // no usable program name
    }
    size_t e = k.find_last_not_of(" \t");
    k = k.substr(b, e - b + 1);
    k += wayland ? "@w" : "@x";
    return k;
}

/// Parse one override value: "auto" (any case) or
/// "paceMs,preCommitMs,postCommitMs" where each field is -1 ("auto") or an
/// integer ms.  Fields above the kMax* bounds are clamped; a field < -1 or
/// a malformed triple rejects the whole value.  False → caller treats as
/// auto.
inline bool parseAppDelayOverride(const std::string &value,
                                  AppDelayOverride &out) {
    if (value == "auto" || value == "Auto" || value == "AUTO") {
        out = AppDelayOverride{};
        return true;
    }
    // Split into exactly three comma-separated integer fields.
    int fields[3] = {-1, -1, -1};
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        size_t comma = value.find(',', start);
        if (i < 2 && comma == std::string::npos) {
            return false; // fewer than three fields
        }
        if (i == 2 && comma != std::string::npos) {
            return false; // trailing garbage: "5,30,0," is malformed
        }
        std::string tok = value.substr(
            start, comma == std::string::npos ? std::string::npos
                                              : comma - start);
        char *end = nullptr;
        long v = strtol(tok.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || v < -1) {
            return false;
        }
        fields[i] = static_cast<int>(v);
        start = comma + 1;
    }
    auto clampTo = [](int v, int max) {
        return v > max ? max : v;
    };
    out.paceMs = clampTo(fields[0], kMaxPaceMs);
    out.preCommitMs = clampTo(fields[1], kMaxPreCommitMs);
    out.postCommitMs = clampTo(fields[2], kMaxPostCommitMs);
    return true;
}

/// "auto" when nothing is set, else "p,pre,post" with -1 for auto fields.
inline std::string formatAppDelayOverride(const AppDelayOverride &o) {
    if (!o.any()) {
        return "auto";
    }
    return std::to_string(o.paceMs) + "," + std::to_string(o.preCommitMs) +
           "," + std::to_string(o.postCommitMs);
}

} // namespace skey

#endif // SKEY_APP_DELAY_KEY_H
