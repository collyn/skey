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
    void autoRestore();

    /// Whether the engine is in English bypass mode (after undo detected).
    bool isEnglishBypass() const { return englishBypass_; }

private:
    /// Recompose from raw input using bamboo-core.
    void recompose();

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
