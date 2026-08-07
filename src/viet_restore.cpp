/**
 * viet_restore.cpp — Auto-restore logic for VietnameseEngine.
 *
 * Two restore paths share a common predicate:
 *
 *   R5  maybeAutoRestoreRealTime  — runs after every recompose() and
 *                                   backspace(); requires all-ASCII result
 *                                   before restoring (preserves intentional
 *                                   Telex transforms like oo→ô mid-word).
 *   --  autoRestore               — commit-time one-shot restore without
 *                                   the all-ASCII check.
 *
 * Both consult bamboo-core's is_valid() and the autoRestore_ flag.
 * The ddFreeStyle guard protects abbreviations where the only non-ASCII
 * character is đ/Đ and there are no vowels (vcđ, đc, nđm).
 *
 * TODO: bamboo-core 0.3.12's is_valid() is more lenient than lotus's Go
 * spelling engine.  Words like "wôd" and "rebôt" pass validation even though
 * they are not valid Vietnamese syllables.  Upstream fix needed before the
 * real-time all-ASCII guard can be removed.
 */

#include "vietnamese.h"

#include "bamboo_ffi.h"
#include "viet_util.h"

namespace skey {

// ---------------------------------------------------------------------------
// Shared predicate
// ---------------------------------------------------------------------------

bool VietnameseEngine::shouldRestoreToRaw(bool requireAllAscii) const {
    // Trigger: autoRestore_ is on, composed_ differs from rawInput_,
    // rawInput_ is non-empty, bamboo says the result is invalid.
    //
    // ddFreeStyle guard: words whose only Vietnamese character is đ/Đ
    // and have no (ASCII) vowels are intentional abbreviations
    // (vcđ, đc, nđm) — never restore them, even at commit time.
    //
    // requireAllAscii is kept as a parameter for API compatibility but
    // no longer restricts real-time restore; both paths use the same
    // predicate since applyFallbackPairSubstitution now runs before
    // is_valid() is checked (moved into recompose R4b).
    //
    // Reads: autoRestore_, rawInput_, composed_, handle_.
    if (!autoRestore_) return false;
    if (rawInput_.empty()) return false;
    if (composed_ == rawInput_) return false;
    if (skey_engine_is_valid(handle_) != 0) return false;

    // ddFreeStyle: abbreviations with đ but no vowels
    if (detail::containsD(composed_) && !detail::hasAsciiVowel(composed_))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// R5 — Real-time auto-restore
// ---------------------------------------------------------------------------

void VietnameseEngine::maybeAutoRestoreRealTime() {
    // Lotus-style real-time restore: if the composition is not valid
    // Vietnamese, restore to rawInput_ immediately.  This handles
    // English words with accidental Telex transforms ("reboot" → "rebôt").
    //
    // Fallback pair substitution (R4b) now runs before this check, so
    // is_valid() sees the final composed_ state including dd→đ, oo→ô etc.
    //
    // Abbreviations with đ but no vowels (vcđ, đc, nđm) are protected
    // by the ddFreeStyle guard in shouldRestoreToRaw().
    //
    // Reads: autoRestore_, rawInput_, composed_, handle_.
    // Writes: composed_ (restored to rawInput_ when condition met).
    if (shouldRestoreToRaw(/*requireAllAscii=*/false)) {
        composed_ = rawInput_;
    }
}

// ---------------------------------------------------------------------------
// Commit-time auto-restore
// ---------------------------------------------------------------------------

void VietnameseEngine::autoRestore() {
    // Commit-time auto-restore without the all-ASCII check.
    // Trigger: autoRestore_ is on, composed_ differs from rawInput_,
    // and bamboo says the result is invalid.
    //
    // ddFreeStyle guard protects đ-abbreviations even at commit time.
    //
    // Reads/writes: autoRestore_, rawInput_, composed_.
    if (shouldRestoreToRaw(/*requireAllAscii=*/false)) {
        composed_ = rawInput_;
    }
}

} // namespace skey
