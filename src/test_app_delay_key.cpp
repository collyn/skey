#include "app_delay_key.h"
#include <cstdlib>
#include <iostream>

static void check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    using namespace skey;

    // ── appDelayKey sanitizer ──
    check(appDelayKey("ghostty", false) == "ghostty@x", "plain key x11");
    check(appDelayKey("ghostty", true) == "ghostty@w", "plain key wayland");
    check(appDelayKey("a/b=c#d;e[f]g\"h", false) == "a_b_c_d_e_f_g_h@x",
          "ini-hostile chars sanitized");
    check(appDelayKey("  ghostty  ", false) == "ghostty@x", "trim");
    check(appDelayKey("   ", false).empty(), "whitespace-only empty");
    check(appDelayKey("", false).empty(), "empty program empty");
    check(appDelayKey("x\n\t\r", false) == "x___@x", "control chars sanitized");

    // ── parseAppDelayOverride ──
    AppDelayOverride ov;
    check(parseAppDelayOverride("auto", ov) && !ov.any(), "auto parses all -1");
    check(parseAppDelayOverride("AUTO", ov) && !ov.any(), "AUTO case");
    check(parseAppDelayOverride("5,30,0", ov) && ov.paceMs == 5 &&
              ov.preCommitMs == 30 && ov.postCommitMs == 0,
          "full triple");
    check(parseAppDelayOverride("5,-1,0", ov) && ov.paceMs == 5 &&
              ov.preCommitMs == -1 && ov.postCommitMs == 0,
          "per-field auto; literal 0 kept");
    check(parseAppDelayOverride("-1,-1,-1", ov) && !ov.any(),
          "all-auto triple");
    check(parseAppDelayOverride("999999,999999,999999", ov) &&
              ov.paceMs == kMaxPaceMs && ov.preCommitMs == kMaxPreCommitMs &&
              ov.postCommitMs == kMaxPostCommitMs,
          "out-of-range clamped");
    check(!parseAppDelayOverride("5,x,0", ov), "non-numeric rejected");
    check(!parseAppDelayOverride("5,30", ov), "two fields rejected");
    check(!parseAppDelayOverride("5,30,0,", ov), "trailing comma rejected");
    check(!parseAppDelayOverride("", ov), "empty rejected");
    check(!parseAppDelayOverride("-2,5,5", ov), "below -1 rejected");

    // ── format round-trip ──
    // "-1,-1,-1" canonicalizes to "auto" (same meaning), so it is checked
    // separately.
    for (const char *v : {"auto", "5,30,0", "5,-1,0", "0,0,0"}) {
        AppDelayOverride parsed;
        check(parseAppDelayOverride(v, parsed), "round-trip parse");
        check(formatAppDelayOverride(parsed) == v, "round-trip format");
    }
    {
        AppDelayOverride parsed;
        check(parseAppDelayOverride("-1,-1,-1", parsed), "all-auto parse");
        check(formatAppDelayOverride(parsed) == "auto",
              "all-auto canonicalizes to auto");
    }

    std::cout << "app_delay_key tests passed\n";
    return 0;
}
