/**
 * viet_fallback.cpp — Fallback pair substitution for VietnameseEngine.
 *
 * Bamboo-core transforms double-letter Telex patterns (dd→đ, oo→ô, aa→â,
 * ee→ê, aw→ă, ow→ơ, uw→ư, ww→ư) ONLY at syllable start (position 0).
 * At other positions the pair is left untransformed in composed_.
 *
 * This module provides Unikey-like free typing: when bamboo produced no
 * Vietnamese chars at all (composed_ == rawInput_), substitute every
 * untransformed pair from a static lookup table.
 *
 * After substitution, the engine state is rebuilt from the modified
 * composed_ so that is_valid() sees the final transformed characters.
 */

#include "vietnamese.h"

#include <string>

#include "bamboo_ffi.h"
#include "viet_util.h"

namespace skey {

// ---------------------------------------------------------------------------
// Static lookup table
// ---------------------------------------------------------------------------

/// Telex two-letter transforms: pair → UTF-8 result.
/// Only applied when composed_ == rawInput_ (bamboo produced no Vietnamese
/// chars, so these pairs were NOT transformed at non-start positions).
static const struct {
    char c0;         // first char (lowercase)
    char c1;         // second char (lowercase)
    const char *lo;  // UTF-8 lowercase result
    const char *up;  // UTF-8 uppercase result
} kTelexPairs[] = {
    {'d','d', "\xC4\x91","\xC4\x90"}, // đ / Đ
    {'o','o', "\xC3\xB4","\xC3\x94"}, // ô / Ô
    {'a','a', "\xC3\xA2","\xC3\x82"}, // â / Â
    {'e','e', "\xC3\xAA","\xC3\x8A"}, // ê / Ê
    {'w','w', "\xC6\xB0","\xC6\xAF"}, // ư / Ư
    {'a','w', "\xC4\x83","\xC4\x82"}, // ă / Ă
    {'o','w', "\xC6\xA1","\xC6\xA0"}, // ơ / Ơ
    {'u','w', "\xC6\xB0","\xC6\xAF"}, // ư / Ư
};

// ---------------------------------------------------------------------------
// P6 — Fallback pair substitution
// ---------------------------------------------------------------------------

void VietnameseEngine::applyFallbackPairSubstitution() {
    // Edge case: bamboo-core transforms double-letter Telex patterns only at
    // syllable start (position 0).  At non-start positions ("aloo"→"alô",
    // "baa"→"bâ"), the pair is left as-is.  This gives Unikey-like free
    // typing by substituting EVERYWHERE when bamboo produced no output.
    //
    // Trigger: composed_ == rawInput_ && composed_.size() >= 2.
    // Undo patterns (ddd, ooo) are excluded because they make
    // composed_ ≠ rawInput_ (bamboo consumed a char for undo).
    //
    // Single left-to-right pass; advance-by-2 on match, advance-by-1
    // otherwise.  First-char uppercase → uppercase UTF-8 result.
    //
    // Reads/writes: composed_.
    // Bamboo limitation: pair-transform-at-position-0-only is the biggest
    // upstream candidate — the wrapper table is removable once bamboo
    // transforms pairs anywhere.

    if (composed_ != rawInput_ || composed_.size() < 2) return;

    std::string fixed;
    for (size_t i = 0; i < composed_.size(); ) {
        bool replaced = false;
        if (i + 1 < composed_.size()) {
            char a = composed_[i];
            char b = composed_[i + 1];
            char al = detail::toLowerASCII(a);
            char bl = detail::toLowerASCII(b);
            bool firstUpper = (a >= 'A' && a <= 'Z');

            for (auto &p : kTelexPairs) {
                if (al == p.c0 && bl == p.c1) {
                    fixed += firstUpper ? p.up : p.lo;
                    i += 2;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            fixed += composed_[i];
            ++i;
        }
    }
    if (fixed == composed_) return;  // no changes

    composed_ = fixed;

    // Rebuild engine state from the modified composed_ so that
    // is_valid() (called by maybeAutoRestoreRealTime after P6)
    // evaluates the final transformed string, not the original
    // bamboo output.
    skey_engine_reset(handle_);
    char *result = skey_engine_process_string(handle_, composed_.c_str());
    if (result) bamboo_free_string(result);
}

} // namespace skey
