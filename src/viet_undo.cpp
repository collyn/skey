/**
 * viet_undo.cpp — Undo and English-bypass logic for VietnameseEngine.
 *
 * Three related concerns that all involve "the user wants to cancel a
 * Telex transform and get raw ASCII back":
 *
 *   P2  processEnglishBypass  — fast path: skip VN processing for the
 *                               remainder of the current word after undo.
 *   P4  tryUndoTransform       — detect ooo→oo, ddd→dd, ww→w cancel patterns.
 *   P5  tryToneKeyUndo         — detect xx→raw-form tone cycle break.
 *
 * Each is a self-contained predicate+action pair extracted from processKey().
 */

#include "vietnamese.h"

#include <string>

#include "viet_util.h"

namespace skey {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

void VietnameseEngine::resetCompositionState() {
    // Clear rawInput_ and composed_.
    // Deliberately does NOT clear committed_ — undo paths (P4, P5) append
    // to it; only reset() clears it.
    rawInput_.clear();
    composed_.clear();
}

void VietnameseEngine::enterEnglishBypass() {
    englishBypass_ = true;
}

// ---------------------------------------------------------------------------
// P2 — English bypass
// ---------------------------------------------------------------------------

bool VietnameseEngine::processEnglishBypass() {
    // Edge case: after an undo set englishBypass_, subsequent keys in the
    // same word must pass through as raw ASCII without Vietnamese processing.
    // Trigger: englishBypass_ == true.
    // Must run AFTER rawInput_ += ch (the appended char is part of the word
    // being bypassed).
    // Reads/writes: englishBypass_, rawInput_, composed_.
    if (!englishBypass_) return false;
    composed_ = rawInput_;
    return true;  // caller returns ProcessResult::Consumed
}

// ---------------------------------------------------------------------------
// P4 — Undo detection (transform cancelled by repeat key)
// ---------------------------------------------------------------------------

bool VietnameseEngine::tryUndoTransform(
    char /*ch*/, const std::string &/*oldComposed*/, const std::string &/*oldRawInput*/) {
    // P4 is now disabled. The skey-engine core handles toggle patterns
    // internally (oo↔ô, dd↔đ, ww↔ư, etc.), so the wrapper no longer
    // needs to detect "undo" by comparing before/after ASCII state.
    return false;
}

// ---------------------------------------------------------------------------
// P5 — Double same-tone undo
// ---------------------------------------------------------------------------

bool VietnameseEngine::tryToneKeyUndo(
    char ch, const std::string &oldComposed, const std::string &oldRawInput) {
    // Double same-tone key = undo to clean raw form.
    // Edge case: bamboo-core treats a repeated tone key as a no-op
    // (ngã + x stays ngã), but users expect "xx" after a tone to reveal
    // the base characters + just one tone key.
    //
    // Trigger: ch is a tone key (s/f/r/x/j/z) AND oldComposed ≠ oldRawInput
    // AND oldComposed == composed_ (key changed nothing) AND
    // lowercase(ch) == lowercase(oldRawInput.back()).
    //
    // Action: build clean base by stripping every tone key AFTER the first
    // vowel from oldRawInput (tone keys before it are letters, e.g. 'x' in
    // "xin"), commit that base, reset engine with rawInput_ = single current
    // tone key, composed_ = that key.
    //
    // Does NOT set englishBypass_ (unlike P4) — the word-level bypass in P4
    // lasts the remainder of the word; the tone key reset here produces a
    // clean state for a fresh tone key.
    //
    // Writes: committed_ (appended), rawInput_, composed_, engine.
    // Bamboo limitation: without the engine reset + rawInput_ shrink, the
    // full old rawInput_ (with prior tone keys) is re-processed and the
    // tone re-appears — creating the unbreakable xìn→xinf→xìn cycle.
    // MUST stay in the wrapper.

    char cl = detail::toLowerASCII(ch);
    if (!detail::isToneKey(cl)) return false;
    if (oldComposed == oldRawInput || oldRawInput.empty()) return false;
    if (oldComposed != composed_) return false;

    char lastCl = detail::toLowerASCII(oldRawInput.back());
    if (cl != lastCl) return false;

    // Build clean raw form: strip all tone keys from oldRawInput,
    // keep base chars + append current tone key.
    size_t firstVowel = detail::findFirstVowel(oldRawInput);
    std::string base;
    for (size_t i = 0; i < oldRawInput.size(); ++i) {
        char c = oldRawInput[i];
        char lc = detail::toLowerASCII(c);
        bool isToneK = detail::isToneKey(lc);
        // Only strip tone keys that appear after the first vowel —
        // tone keys before the vowel are regular letters.
        if (isToneK &&
            firstVowel != std::string::npos &&
            static_cast<int>(i) > static_cast<int>(firstVowel))
            continue;
        base += c;
    }
    // Commit the clean base and reset engine so subsequent keys start
    // from the current tone key as a regular letter.
    committed_ = base;
    rawInput_ = std::string(1, ch);
    composed_ = rawInput_;

    return true;  // caller returns ProcessResult::Committed
}

} // namespace skey
