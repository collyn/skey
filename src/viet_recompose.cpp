/**
 * viet_recompose.cpp — recompose() pipeline for VietnameseEngine.
 *
 * Every keystroke (and backspace) triggers recompose(), which rebuilds the
 * full word composition from rawInput_ through these five steps:
 *
 *   R1  translateBracketUO   – input-layout mapping ('[' → "ow", ']' → "uw")
 *   R2  findFirstVowel       – vowel-boundary detection (see viet_util.h)
 *   R3  deduplicateToneKeys  – tone-key dedup (see viet_util.h)
 *   R4  runBamboo             – core FFI call to bamboo-core
 *
 * Steps R1–R3 are pure (no state mutation); R4 writes composed_.
 * R5 (maybeAutoRestoreRealTime) is called after P6 in processKey().
 */

#include "vietnamese.h"

#include <cstring>
#include <string>

#include "bamboo_ffi.h"
#include "viet_util.h"

namespace skey {

// ---------------------------------------------------------------------------
// R1 — BracketUO translation
// ---------------------------------------------------------------------------

std::string VietnameseEngine::translateBracketUO() const {
    // Edge case: with the "][→ươ" option, users type '[' for ơ and ']'
    // for ư, but bamboo only understands Telex spellings "ow"/"uw".
    // Trigger: bracketUO_ && method_ == Telex && rawInput_ contains '[' or ']'.
    // Reads: rawInput_, bracketUO_, method_.  Writes: nothing (pure).
    // Bamboo limitation: none — this is an input-layout mapping layer,
    // must stay in the wrapper.
    if (bracketUO_ && method_ == InputMethod::Telex &&
        (rawInput_.find('[') != std::string::npos ||
         rawInput_.find(']') != std::string::npos)) {
        std::string result;
        result.reserve(rawInput_.size() + 4);
        for (char c : rawInput_) {
            if (c == '[')      result += "ow";
            else if (c == ']') result += "uw";
            else               result += c;
        }
        return result;
    }
    return rawInput_;
}

// ---------------------------------------------------------------------------
// R3 — Tone-key dedup (member wrapper)
// ---------------------------------------------------------------------------

std::string VietnameseEngine::deduplicateToneKeys(
    const std::string &input, size_t firstVowel) const {
    // Edge case: bamboo-core fails to compose "looixfsx" — x…x with f,s
    // between.  In Telex each new tone REPLACES the previous, so only the
    // last occurrence matters.
    // Consecutive same-tone pairs ("xx", "ss") are PRESERVED — they are
    // intentional undo patterns handled by tryToneKeyUndo().
    // Reads: input (pure; no state).
    // Bamboo limitation: bamboo-core mis-handles non-consecutive repeated
    // tone keys — this is the strongest upstream candidate.
    return detail::deduplicateToneKeys(input, firstVowel);
}

// ---------------------------------------------------------------------------
// R4 — Bamboo-core call
// ---------------------------------------------------------------------------

void VietnameseEngine::runBamboo(const std::string &dedupedInput) {
    // Feed the deduplicated string to bamboo-core and cache the result in
    // composed_.  skey_engine_process_string() resets the engine then
    // replays the string char-by-char.
    // Writes: composed_ (only state).
    // On FFI failure: fall back to rawInput_.
    char *result = skey_engine_process_string(handle_, dedupedInput.c_str());
    if (result) {
        composed_ = result;
        bamboo_free_string(result);
    } else {
        composed_ = rawInput_;
    }
}

// ---------------------------------------------------------------------------
// recompose() — main pipeline
// ---------------------------------------------------------------------------

void VietnameseEngine::recompose() {
    std::string bambooInput = translateBracketUO();                 // R1
    size_t firstVowel = detail::findFirstVowel(bambooInput);       // R2
    std::string deduped = deduplicateToneKeys(bambooInput, firstVowel); // R3
    runBamboo(deduped);                                             // R4
    // R5 (maybeAutoRestoreRealTime) is called in processKey() after
    // applyFallbackPairSubstitution, so is_valid() sees the final state.
}

} // namespace skey
