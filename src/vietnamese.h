#ifndef FCITX5_SKEY_VIETNAMESE_H
#define FCITX5_SKEY_VIETNAMESE_H

#include <string>
#include <vector>

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

/// Tone types in Vietnamese
enum class Tone {
    None = 0, // Thanh ngang (no mark)
    Sac,      // Dấu sắc (´)
    Huyen,    // Dấu huyền (`)
    Hoi,      // Dấu hỏi (hook above)
    Nga,      // Dấu ngã (~)
    Nang,     // Dấu nặng (dot below)
};

/// Diacritical mark types (vowel modifications)
enum class Mark {
    None = 0,
    Hat,    // Circumflex: a→â, e→ê, o→ô
    Breve,  // Breve: a→ă
    Horn,   // Horn: o→ơ, u→ư
    Stroke, // Stroke: d→đ
};

/// Represents a Vietnamese character with its base, mark, and tone
struct VChar {
    char base = 0;         // Base letter (a,e,i,o,u,y,d)
    Mark mark = Mark::None;
    Tone tone = Tone::None;
    bool upper = false;    // Uppercase?

    /// Convert to UTF-8 string
    std::string toUtf8() const;

    /// Check if this is a vowel
    bool isVowel() const;

    /// Check if this character can accept the given mark
    bool canAcceptMark(Mark m) const;
};

/// Core Vietnamese input processing engine.
///
/// Maintains composition state for a single syllable and handles
/// Telex/VNI input rules, tone placement, and mark application.
class VietnameseEngine {
public:
    VietnameseEngine();

    // Configuration
    void setMethod(InputMethod method) { method_ = method; }
    void setToneStyle(ToneStyle style) { toneStyle_ = style; }
    void setFreeMarking(bool free) { freeMarking_ = free; }

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

private:
    /// Recompose from raw input.
    void recompose();

    /// Try to apply a Telex modifier key.
    bool applyTelexModifier(char ch);

    /// Try to apply a VNI modifier key.
    bool applyVNIModifier(char ch);

    /// Apply a tone to the current syllable.
    bool applyTone(Tone tone);

    /// Apply a diacritical mark.
    bool applyMark(Mark mark, char target = 0);

    /// Remove the current tone.
    bool removeTone();

    /// Remove diacritical marks.
    bool removeMark();

    /// Find the vowel index where the tone should be placed.
    int findToneTarget() const;

    /// Parse syllable structure from the chars_ vector.
    void parseSyllable();

    // State
    InputMethod method_ = InputMethod::Telex;
    ToneStyle toneStyle_ = ToneStyle::Modern;
    bool freeMarking_ = true;

    std::string rawInput_;       // What the user actually typed
    std::vector<VChar> chars_;   // Parsed character array
    std::string committed_;      // Auto-committed text

    // Syllable structure indices (into chars_)
    int vowelStart_ = -1;  // First vowel index
    int vowelEnd_ = -1;    // Last vowel index (inclusive)
    int consonantEnd_ = -1; // End of final consonant
};

// Utility functions

/// Convert a Vietnamese character (base + mark + tone) to its UTF-8 representation.
std::string vietnameseCharToUtf8(char base, Mark mark, Tone tone, bool upper);

/// Check if a character is a Vietnamese vowel letter.
bool isVietnameseVowel(char ch);

/// Check if a character is a Vietnamese consonant letter.
bool isVietnameseConsonant(char ch);

} // namespace skey

#endif // FCITX5_SKEY_VIETNAMESE_H
