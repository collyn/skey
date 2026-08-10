/**
 * viet_restore.cpp — Auto-restore logic for VietnameseEngine.
 *
 * Two restore paths share a common predicate:
 *
 *   R5  maybeAutoRestoreRealTime  — runs after recompose() in processKey();
 *                                   lotus-style real-time restore based on
 *                                   string-based is_valid().
 *   --  autoRestore               — commit-time one-shot restore, same
 *                                   predicate.
 *
 * Both use skey_engine_is_valid() which works on the composed string
 * directly (not on engine state).  Pair substitution is built into
 * skey-engine, so is_valid() always sees the final composed form.
 *
 * The ddFreeStyle guard protects abbreviations where the only non-ASCII
 * character is đ/Đ and there are no vowels (vcđ, đc, nđm).
 */

#include "vietnamese.h"

#include "skey_engine.h"
#include "viet_util.h"

namespace skey {

// ---------------------------------------------------------------------------
// Shared predicate
// ---------------------------------------------------------------------------

bool VietnameseEngine::shouldRestoreToRaw() const {
    if (!autoRestore_) return false;
    if (rawInput_.empty()) return false;
    if (composed_ == rawInput_) return false;
    if (skey_engine_is_valid(composed_.c_str()) != 0) return false;

    // ddFreeStyle: abbreviations and dd→đ transforms with no vowels.
    // Check rawInput_ (not composed_) because composed_ may contain
    // non-ASCII vowels like ả/ấ/ô that hasAsciiVowel skips, causing
    // false positives (e.g. "addr" → composed "ảđ" has no ASCII vowel
    // but rawInput_ "addr" clearly has 'a').
    //
    // Also protect dd toggle-back results (e.g. "ddd"→"dd", "dadd"→"dad"
    // where đ was toggled back to d). In these cases composed has no đ
    // so containsD() alone is insufficient — but the rawInput has 3+ 'd'
    // characters, indicating a toggle pattern in progress.
    if (!detail::hasAsciiVowel(rawInput_)) {
        if (detail::containsD(composed_)) return false;
        // Protect dd toggle-back: rawInput has d but no vowel
        for (char c : rawInput_) {
            if (c == 'd' || c == 'D') return false;
        }
    }
    // Protect dd toggle-back when rawInput HAS a vowel but has 3+ d's
    // (e.g. "dadd"→"dad" — the third d toggled đ back to d).
    {
        int dCount = 0;
        for (char c : rawInput_) {
            if (c == 'd' || c == 'D') dCount++;
        }
        if (dCount >= 3) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// R5 — Real-time auto-restore
// ---------------------------------------------------------------------------

void VietnameseEngine::maybeAutoRestoreRealTime() {
    // Lotus-style real-time restore: if the composition is not valid
    // Vietnamese, restore to rawInput_ immediately.
    //
    // Pair substitution is built into skey-engine, so is_valid() sees
    // the final composed state including dd→đ, oo→ô etc.
    //
    // Abbreviations with đ but no vowels (vcđ, đc, nđm) are protected
    // by the ddFreeStyle guard.
    if (shouldRestoreToRaw()) {
        composed_ = rawInput_;
    }
}

// ---------------------------------------------------------------------------
// Commit-time auto-restore
// ---------------------------------------------------------------------------

void VietnameseEngine::autoRestore() {
    if (shouldRestoreToRaw()) {
        composed_ = rawInput_;
    }
}

} // namespace skey
