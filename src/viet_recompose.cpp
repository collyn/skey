/**
 * viet_recompose.cpp — recompose() pipeline for VietnameseEngine.
 *
 * Every keystroke (and backspace) triggers recompose(), which rebuilds the
 * full word composition from rawInput_:
 *
 *   R1  translateBracketUO   – input-layout mapping ('[' → "ow", ']' → "uw")
 *   R2  runSkeyEngine         – core FFI call to skey-engine (built-in pair
 *                                substitution and tone-key dedup)
 *
 * R5 (maybeAutoRestoreRealTime) is called after recompose in processKey().
 */

#include "vietnamese.h"

#include <cstring>
#include <string>

#include "skey_engine.h"
#include "viet_util.h"

namespace skey {

// ---------------------------------------------------------------------------
// R1 — BracketUO translation
// ---------------------------------------------------------------------------

std::string VietnameseEngine::translateBracketUO() const {
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
// R2 — Viet-engine call
// ---------------------------------------------------------------------------

void VietnameseEngine::runSkeyEngine(const std::string &input) {
    char *result = skey_engine_transform(handle_, input.c_str());
    if (result) {
        composed_ = result;
        skey_free_string(result);
    } else {
        composed_ = rawInput_;
    }
}

// ---------------------------------------------------------------------------
// recompose() — main pipeline
// ---------------------------------------------------------------------------

void VietnameseEngine::recompose() {
    std::string engineInput = translateBracketUO();               // R1
    runSkeyEngine(engineInput);                                    // R2
}

} // namespace skey
