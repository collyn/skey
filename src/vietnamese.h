#ifndef FCITX5_SKEY_VIETNAMESE_H
#define FCITX5_SKEY_VIETNAMESE_H

#include <string>

// Forward declare opaque handle from bamboo FFI
extern "C" {
    typedef void BambooEngine;
}

namespace skey {

/// Input method type
enum class InputMethod { Telex, VNI };

/// Tone mark position style
enum class ToneStyle { Modern, Traditional };

/// Result of processing a key
enum class ProcessResult {
    Consumed,   // Key was consumed, preedit updated
    Committed,  // Previous buffer was committed, new composition started
    Ignored,    // Key was not relevant
};

/// Core Vietnamese input processing engine.
///
/// Thin wrapper around bamboo-core (Rust) via C FFI.
/// Maintains composition state for a single syllable and handles
/// Telex/VNI input rules via bamboo-core's optimized engine.
class VietnameseEngine {
public:
    VietnameseEngine();
    ~VietnameseEngine();

    // Non-copyable (owns opaque Rust handle)
    VietnameseEngine(const VietnameseEngine &) = delete;
    VietnameseEngine &operator=(const VietnameseEngine &) = delete;

    // Movable
    VietnameseEngine(VietnameseEngine &&other) noexcept;
    VietnameseEngine &operator=(VietnameseEngine &&other) noexcept;

    // Configuration
    void setMethod(InputMethod method);
    void setToneStyle(ToneStyle style);
    void setFreeMarking(bool free);
    void setAutoRestore(bool restore);
    /// Telex only: bare 'w' → 'ư' (switches bamboo to telex_w).
    void setShortW(bool enabled);
    /// Telex only: '[' → 'ơ', ']' → 'ư' (translated to ow/uw for bamboo).
    void setBracketUO(bool enabled);

    /// Process a single key press. Returns the result type.
    ProcessResult processKey(char ch);

    /// Handle backspace: remove last raw input character and recompose.
    void backspace();

    /// Reset all state.
    void reset();

    /// Get the current composed (Vietnamese) text.
    std::string getComposed() const;

    /// Get the raw input buffer.
    const std::string &getRawInput() const { return rawInput_; }

    /// Get text that was committed (for auto-commit scenarios).
    const std::string &getCommitted() const { return committed_; }

    /// Clear the committed buffer.
    void clearCommitted() { committed_.clear(); }

    /// Auto-restore: if current composition is not valid Vietnamese,
    /// replace composed text with raw input. Call before committing.
    /// Uses the same unified predicate as maybeAutoRestoreRealTime().
    void autoRestore();

    /// Whether the engine is in English bypass mode (after undo detected).
    bool isEnglishBypass() const { return englishBypass_; }

    /// Clear English bypass at word boundaries so the next word
    /// starts with fresh Vietnamese processing.
    void clearEnglishBypass() { englishBypass_ = false; }

    /// Check if the current composition forms a valid Vietnamese syllable.
    /// Returns false for English words that accidentally trigger tone keys
    /// (e.g. "ultr" → "ủlt" is not valid Vietnamese).
    bool isValid() const;

private:
    /// Recompose from raw input using bamboo-core.
    void recompose();

    // ── recompose() pipeline steps ─────────────────────────────────────
    //
    // Each step compensates for a bamboo-core limitation or implements an
    // skey-specific input-mapping feature.

    /// Translate '[' → "ow" and ']' → "uw" for the BracketUO option.
    /// Edge case: bamboo only understands Telex spellings "ow"/"uw", not
    /// bracket keys.  Applies only when bracketUO_ is on in Telex mode.
    /// Reads: rawInput_, bracketUO_, method_.  Pure — returns new string.
    std::string translateBracketUO() const;

    /// Deduplicate repeated tone keys.  Delegates to
    /// detail::deduplicateToneKeys for the scan; wraps with the recompose
    /// context (method check).
    /// Reads: nothing beyond input.  Pure — returns new string.
    std::string deduplicateToneKeys(const std::string &input,
                                    size_t firstVowel) const;

    /// Feed the deduplicated input to bamboo-core and cache the result
    /// in composed_.  On FFI failure, falls back to rawInput_.
    /// Writes: composed_.
    void runBamboo(const std::string &dedupedInput);

    // ── processKey() undo / fallback / restore helpers ─────────────────

    /// Is this key relevant to composition?  Letters, VNI digits (after a
    /// vowel), and bracket keys (when bracketUO_ is on in Telex).
    /// Returns false → caller should return ProcessResult::Ignored.
    bool isCompositionKey(char ch) const;

    /// English bypass fast path: after an undo was detected, skip
    /// Vietnamese processing for the remainder of the word.
    /// Returns true if bypass consumed the key.
    bool processEnglishBypass();

    /// Undo detection: a transform (oo→ô, dd→đ, w→ư) is cancelled by
    /// repeating its trigger key (ooo→oo, ddd→dd, ww→w).
    /// Returns true if an undo was detected and handled.
    bool tryUndoTransform(char ch,
                          const std::string &oldComposed,
                          const std::string &oldRawInput);

    /// Double same-tone undo: pressing the same tone key twice (e.g. "xx")
    /// undoes the tone and reveals the clean raw form.
    /// Returns true if handled.
    bool tryToneKeyUndo(char ch,
                        const std::string &oldComposed,
                        const std::string &oldRawInput);

    /// Clear rawInput_, composed_, and reset the bamboo engine.
    /// Deliberately does NOT clear committed_ — undo paths append to it.
    void resetCompositionState();

    /// Set englishBypass_ = true.  Called only by tryUndoTransform().
    void enterEnglishBypass();

    /// Fallback pair substitution: bamboo-core only transforms double-letter
    /// Telex patterns at syllable start.  Replace untransformed pairs
    /// (dd→đ, oo→ô, aa→â, ee→ê, aw→ă, ow→ơ, uw→ư, ww→ư) at any position.
    void applyFallbackPairSubstitution();

    // ── Auto-restore ───────────────────────────────────────────────────

    /// Real-time auto-restore after every recompose (also from backspace).
    /// Prevents Telex modifier keys from destructively rewriting English
    /// words (e.g. "address" → bamboo yields "addres" when ss = undo tone).
    /// Only restores when composed_ is all-ASCII.
    /// Abbreviations with đ but no vowels are protected by ddFreeStyle.
    void maybeAutoRestoreRealTime();

    /// Shared predicate: should composed_ be restored to rawInput_?
    /// When requireAllAscii is true (real-time path), only restores
    /// all-ASCII results — Vietnamese transforms like oo→ô are preserved.
    /// When false (commit-time path), restores any invalid result.
    /// ddFreeStyle guard: words with đ but no vowels (vcđ, đc, nđm)
    /// are never restored.
    bool shouldRestoreToRaw(bool requireAllAscii) const;

    BambooEngine *handle_ = nullptr;

    InputMethod method_ = InputMethod::Telex;
    ToneStyle toneStyle_ = ToneStyle::Modern;
    bool freeMarking_ = false;
    bool autoRestore_ = true;
    bool shortW_ = false;      // Telex: bare 'w' → 'ư' (telex_w)
    bool bracketUO_ = false;   // Telex: '[' → 'ơ', ']' → 'ư'

    std::string rawInput_;       // What the user actually typed
    std::string composed_;       // Cached composed output from bamboo-core
    bool englishBypass_ = false;  // After undo, skip Vietnamese processing
    std::string committed_;      // Auto-committed text
};

} // namespace skey

#endif // FCITX5_SKEY_VIETNAMESE_H
