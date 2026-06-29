#include "vietnamese.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace skey {

// ============================================================================
// Unicode lookup table for Vietnamese characters
// ============================================================================
//
// Table layout: [base_vowel][mark][tone] → Unicode code point
// base_vowel index: a=0, e=1, i=2, o=3, u=4, y=5
// mark index: None=0, Hat(circumflex)=1, Breve=2, Horn=3
// tone index: None=0, Sac=1, Huyen=2, Hoi=3, Nga=4, Nang=5
//
// Value of 0 means invalid combination.

// clang-format off
static const uint32_t kVowelTable[6][4][6] = {
    // a: base=0
    {
        // Mark::None
        { 'a',    0x00E1, 0x00E0, 0x1EA3, 0x00E3, 0x1EA1 },
        // Mark::Hat (â)
        { 0x00E2, 0x1EA5, 0x1EA7, 0x1EA9, 0x1EAB, 0x1EAD },
        // Mark::Breve (ă)
        { 0x0103, 0x1EAF, 0x1EB1, 0x1EB3, 0x1EB5, 0x1EB7 },
        // Mark::Horn (not valid for 'a')
        { 0, 0, 0, 0, 0, 0 },
    },
    // e: base=1
    {
        // Mark::None
        { 'e',    0x00E9, 0x00E8, 0x1EBB, 0x1EBD, 0x1EB9 },
        // Mark::Hat (ê)
        { 0x00EA, 0x1EBF, 0x1EC1, 0x1EC3, 0x1EC5, 0x1EC7 },
        // Mark::Breve (not valid for 'e')
        { 0, 0, 0, 0, 0, 0 },
        // Mark::Horn (not valid for 'e')
        { 0, 0, 0, 0, 0, 0 },
    },
    // i: base=2
    {
        // Mark::None
        { 'i',    0x00ED, 0x00EC, 0x1EC9, 0x0129, 0x1ECB },
        // Mark::Hat (not valid for 'i')
        { 0, 0, 0, 0, 0, 0 },
        // Mark::Breve (not valid for 'i')
        { 0, 0, 0, 0, 0, 0 },
        // Mark::Horn (not valid for 'i')
        { 0, 0, 0, 0, 0, 0 },
    },
    // o: base=3
    {
        // Mark::None
        { 'o',    0x00F3, 0x00F2, 0x1ECF, 0x00F5, 0x1ECD },
        // Mark::Hat (ô)
        { 0x00F4, 0x1ED1, 0x1ED3, 0x1ED5, 0x1ED7, 0x1ED9 },
        // Mark::Breve (not valid for 'o')
        { 0, 0, 0, 0, 0, 0 },
        // Mark::Horn (ơ)
        { 0x01A1, 0x1EDB, 0x1EDD, 0x1EDF, 0x1EE1, 0x1EE3 },
    },
    // u: base=4
    {
        // Mark::None
        { 'u',    0x00FA, 0x00F9, 0x1EE7, 0x0169, 0x1EE5 },
        // Mark::Hat (not valid for 'u')
        { 0, 0, 0, 0, 0, 0 },
        // Mark::Breve (not valid for 'u')
        { 0, 0, 0, 0, 0, 0 },
        // Mark::Horn (ư)
        { 0x01B0, 0x1EE9, 0x1EEB, 0x1EED, 0x1EEF, 0x1EF1 },
    },
    // y: base=5
    {
        // Mark::None
        { 'y',    0x00FD, 0x1EF3, 0x1EF7, 0x1EF9, 0x1EF5 },
        // Mark::Hat (not valid for 'y')
        { 0, 0, 0, 0, 0, 0 },
        // Mark::Breve (not valid for 'y')
        { 0, 0, 0, 0, 0, 0 },
        // Mark::Horn (not valid for 'y')
        { 0, 0, 0, 0, 0, 0 },
    },
};

// Uppercase versions
static const uint32_t kVowelTableUpper[6][4][6] = {
    // A: base=0
    {
        { 'A',    0x00C1, 0x00C0, 0x1EA2, 0x00C3, 0x1EA0 },
        { 0x00C2, 0x1EA4, 0x1EA6, 0x1EA8, 0x1EAA, 0x1EAC },
        { 0x0102, 0x1EAE, 0x1EB0, 0x1EB2, 0x1EB4, 0x1EB6 },
        { 0, 0, 0, 0, 0, 0 },
    },
    // E: base=1
    {
        { 'E',    0x00C9, 0x00C8, 0x1EBA, 0x1EBC, 0x1EB8 },
        { 0x00CA, 0x1EBE, 0x1EC0, 0x1EC2, 0x1EC4, 0x1EC6 },
        { 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    },
    // I: base=2
    {
        { 'I',    0x00CD, 0x00CC, 0x1EC8, 0x0128, 0x1ECA },
        { 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    },
    // O: base=3
    {
        { 'O',    0x00D3, 0x00D2, 0x1ECE, 0x00D5, 0x1ECC },
        { 0x00D4, 0x1ED0, 0x1ED2, 0x1ED4, 0x1ED6, 0x1ED8 },
        { 0, 0, 0, 0, 0, 0 },
        { 0x01A0, 0x1EDA, 0x1EDC, 0x1EDE, 0x1EE0, 0x1EE2 },
    },
    // U: base=4
    {
        { 'U',    0x00DA, 0x00D9, 0x1EE6, 0x0168, 0x1EE4 },
        { 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
        { 0x01AF, 0x1EE8, 0x1EEA, 0x1EEC, 0x1EEE, 0x1EF0 },
    },
    // Y: base=5
    {
        { 'Y',    0x00DD, 0x1EF2, 0x1EF6, 0x1EF8, 0x1EF4 },
        { 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    },
};
// clang-format on

// ============================================================================
// Utility functions
// ============================================================================

/// Encode a Unicode code point as UTF-8
static std::string codePointToUtf8(uint32_t cp) {
    std::string result;
    if (cp < 0x80) {
        result += static_cast<char>(cp);
    } else if (cp < 0x800) {
        result += static_cast<char>(0xC0 | (cp >> 6));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        result += static_cast<char>(0xE0 | (cp >> 12));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        result += static_cast<char>(0xF0 | (cp >> 18));
        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return result;
}

/// Get the vowel base index (0-5) for the lookup table, or -1
static int vowelBaseIndex(char ch) {
    switch (std::tolower(ch)) {
    case 'a': return 0;
    case 'e': return 1;
    case 'i': return 2;
    case 'o': return 3;
    case 'u': return 4;
    case 'y': return 5;
    default: return -1;
    }
}

bool isVietnameseVowel(char ch) { return vowelBaseIndex(ch) >= 0; }

bool isVietnameseConsonant(char ch) {
    char lower = std::tolower(ch);
    // Vietnamese consonant letters
    return std::strchr("bcdfghjklmnpqrstvwxz", lower) != nullptr;
}

std::string vietnameseCharToUtf8(char base, Mark mark, Tone tone, bool upper) {
    int bi = vowelBaseIndex(base);
    if (bi < 0) {
        // Not a vowel - handle 'd' → 'đ'
        if (std::tolower(base) == 'd' && mark == Mark::Stroke) {
            return upper ? codePointToUtf8(0x0110) : codePointToUtf8(0x0111);
        }
        // Return the base character as-is
        std::string s;
        s += base;
        return s;
    }

    int mi = static_cast<int>(mark);
    int ti = static_cast<int>(tone);

    // Validate indices
    if (mi < 0 || mi > 3 || ti < 0 || ti > 5) {
        std::string s;
        s += base;
        return s;
    }

    const auto &table = upper ? kVowelTableUpper : kVowelTable;
    uint32_t cp = table[bi][mi][ti];

    if (cp == 0) {
        // Invalid combination - return base character
        std::string s;
        s += base;
        return s;
    }

    return codePointToUtf8(cp);
}

// ============================================================================
// VChar implementation
// ============================================================================

std::string VChar::toUtf8() const {
    if (base == 0) return "";

    // Handle 'd' → 'đ'
    if (std::tolower(base) == 'd') {
        if (mark == Mark::Stroke) {
            return upper ? codePointToUtf8(0x0110) : codePointToUtf8(0x0111);
        }
        std::string s;
        s += (upper ? 'D' : 'd');
        return s;
    }

    // Handle vowels
    int bi = vowelBaseIndex(base);
    if (bi >= 0) {
        return vietnameseCharToUtf8(base, mark, tone, upper);
    }

    // Non-vowel, non-d consonant
    std::string s;
    s += (upper ? std::toupper(base) : std::tolower(base));
    return s;
}

bool VChar::isVowel() const { return isVietnameseVowel(base); }

bool VChar::canAcceptMark(Mark m) const {
    if (!isVowel()) {
        return m == Mark::Stroke && std::tolower(base) == 'd';
    }
    int bi = vowelBaseIndex(base);
    if (bi < 0) return false;
    int mi = static_cast<int>(m);
    if (mi < 0 || mi > 3) return false;
    // Check if there's at least one non-zero entry in the table row
    const auto &table = upper ? kVowelTableUpper : kVowelTable;
    return table[bi][mi][0] != 0;
}

// ============================================================================
// VietnameseEngine implementation
// ============================================================================

VietnameseEngine::VietnameseEngine() {}

void VietnameseEngine::reset() {
    rawInput_.clear();
    chars_.clear();
    committed_.clear();
    vowelStart_ = vowelEnd_ = consonantEnd_ = -1;
}

ProcessResult VietnameseEngine::processKey(char ch) {
    rawInput_ += ch;
    recompose();
    return ProcessResult::Consumed;
}

void VietnameseEngine::backspace() {
    if (rawInput_.empty()) return;
    rawInput_.pop_back();
    if (rawInput_.empty()) {
        chars_.clear();
        vowelStart_ = vowelEnd_ = consonantEnd_ = -1;
    } else {
        recompose();
    }
}

std::string VietnameseEngine::getComposed() const {
    std::string result;
    for (const auto &vc : chars_) {
        result += vc.toUtf8();
    }
    return result;
}

// ============================================================================
// Recompose: rebuild chars_ from rawInput_
// ============================================================================

void VietnameseEngine::recompose() {
    chars_.clear();
    vowelStart_ = vowelEnd_ = consonantEnd_ = -1;

    if (rawInput_.empty()) return;

    // Step 1: Parse raw input into basic VChars (no marks/tones yet)
    std::vector<VChar> tempChars;
    std::string pendingModifiers; // Telex modifiers like 's','f','r','x','j','z','w'

    for (size_t i = 0; i < rawInput_.size(); ++i) {
        char ch = rawInput_[i];
        char lower = std::tolower(ch);
        bool upper = std::isupper(ch);

        if (method_ == InputMethod::Telex) {
            // Check for Telex doubling: aa→â, ee→ê, oo→ô, dd→đ
            if (!tempChars.empty()) {
                char prevLower = std::tolower(tempChars.back().base);

                // dd → đ
                if (lower == 'd' && prevLower == 'd' &&
                    tempChars.back().mark == Mark::None) {
                    tempChars.back().mark = Mark::Stroke;
                    continue;
                }

                // aa→â, ee→ê, oo→ô
                if (lower == prevLower && tempChars.back().isVowel() &&
                    tempChars.back().mark == Mark::None) {
                    if (lower == 'a' || lower == 'e' || lower == 'o') {
                        tempChars.back().mark = Mark::Hat;
                        continue;
                    }
                }

                // aw → ă
                if (lower == 'w' && prevLower == 'a' &&
                    tempChars.back().mark == Mark::None) {
                    tempChars.back().mark = Mark::Breve;
                    continue;
                }

                // ow → ơ
                if (lower == 'w' && prevLower == 'o' &&
                    tempChars.back().mark == Mark::None) {
                    tempChars.back().mark = Mark::Horn;
                    continue;
                }

                // uw → ư
                if (lower == 'w' && prevLower == 'u' &&
                    tempChars.back().mark == Mark::None) {
                    tempChars.back().mark = Mark::Horn;
                    continue;
                }
            }

            // Telex tone modifiers: s, f, r, x, j, z
            // These only apply if we already have vowels in the buffer
            bool hasVowels = false;
            for (const auto &tc : tempChars) {
                if (tc.isVowel()) {
                    hasVowels = true;
                    break;
                }
            }

            if (hasVowels) {
                Tone tone = Tone::None;
                bool isToneKey = false;
                switch (lower) {
                case 's': tone = Tone::Sac; isToneKey = true; break;
                case 'f': tone = Tone::Huyen; isToneKey = true; break;
                case 'r': tone = Tone::Hoi; isToneKey = true; break;
                case 'x': tone = Tone::Nga; isToneKey = true; break;
                case 'j': tone = Tone::Nang; isToneKey = true; break;
                case 'z': tone = Tone::None; isToneKey = true; break;
                default: break;
                }

                if (isToneKey) {
                    // Check if the same tone is already applied (undo)
                    bool alreadyApplied = false;
                    if (lower != 'z') {
                        for (const auto &tc : tempChars) {
                            if (tc.isVowel() && tc.tone == tone) {
                                alreadyApplied = true;
                                break;
                            }
                        }
                    }

                    if (alreadyApplied) {
                        // Undo: remove tone and add the letter literally
                        for (auto &tc : tempChars) {
                            if (tc.isVowel()) {
                                tc.tone = Tone::None;
                            }
                        }
                        VChar vc;
                        vc.base = lower;
                        vc.upper = upper;
                        tempChars.push_back(vc);
                    } else {
                        // Apply tone to the correct vowel
                        // First, remove any existing tone
                        for (auto &tc : tempChars) {
                            if (tc.isVowel()) {
                                tc.tone = Tone::None;
                            }
                        }
                        if (lower == 'z') {
                            // Just remove tone, don't add anything
                        } else {
                            // Find tone target and apply
                            // We need to find the right vowel
                            chars_ = tempChars;
                            parseSyllable();
                            int target = findToneTarget();
                            chars_.clear();

                            if (target >= 0 &&
                                target < static_cast<int>(tempChars.size())) {
                                tempChars[target].tone = tone;
                            } else {
                                // Fallback: apply to last vowel
                                for (int k = static_cast<int>(tempChars.size()) - 1;
                                     k >= 0; --k) {
                                    if (tempChars[k].isVowel()) {
                                        tempChars[k].tone = tone;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    continue;
                }

                // 'w' as standalone (after non-applicable contexts):
                // try to apply horn to the last o/u
                if (lower == 'w') {
                    bool applied = false;
                    for (int k = static_cast<int>(tempChars.size()) - 1;
                         k >= 0; --k) {
                        char b = std::tolower(tempChars[k].base);
                        if ((b == 'o' || b == 'u') &&
                            tempChars[k].mark == Mark::None) {
                            tempChars[k].mark = Mark::Horn;
                            applied = true;
                            break;
                        }
                    }
                    if (applied) continue;
                    // If can't apply, treat as literal 'w'
                }
            }
        } else if (method_ == InputMethod::VNI) {
            // VNI mode: digits modify the previous vowel/consonant
            bool hasVowels = false;
            for (const auto &tc : tempChars) {
                if (tc.isVowel()) {
                    hasVowels = true;
                    break;
                }
            }

            if (!tempChars.empty()) {
                bool handled = false;
                switch (ch) {
                case '1': case '2': case '3': case '4': case '5': {
                    // Tone marks (only if there are vowels)
                    if (hasVowels) {
                        Tone tone;
                        switch (ch) {
                        case '1': tone = Tone::Sac; break;
                        case '2': tone = Tone::Huyen; break;
                        case '3': tone = Tone::Hoi; break;
                        case '4': tone = Tone::Nga; break;
                        case '5': tone = Tone::Nang; break;
                        default: tone = Tone::None; break;
                        }
                        // Remove existing tones
                        for (auto &tc : tempChars) {
                            if (tc.isVowel()) tc.tone = Tone::None;
                        }
                        // Find target and apply
                        chars_ = tempChars;
                        parseSyllable();
                        int target = findToneTarget();
                        chars_.clear();
                        if (target >= 0 &&
                            target < static_cast<int>(tempChars.size())) {
                            tempChars[target].tone = tone;
                        } else {
                            for (int k = static_cast<int>(tempChars.size()) - 1;
                                 k >= 0; --k) {
                                if (tempChars[k].isVowel()) {
                                    tempChars[k].tone = tone;
                                    break;
                                }
                            }
                        }
                        handled = true;
                    }
                    break;
                }
                case '6': {
                    // Circumflex: a→â, e→ê, o→ô
                    for (int k = static_cast<int>(tempChars.size()) - 1;
                         k >= 0; --k) {
                        char b = std::tolower(tempChars[k].base);
                        if ((b == 'a' || b == 'e' || b == 'o') &&
                            tempChars[k].mark == Mark::None) {
                            tempChars[k].mark = Mark::Hat;
                            handled = true;
                            break;
                        }
                    }
                    break;
                }
                case '7': {
                    // Horn: o→ơ, u→ư
                    for (int k = static_cast<int>(tempChars.size()) - 1;
                         k >= 0; --k) {
                        char b = std::tolower(tempChars[k].base);
                        if ((b == 'o' || b == 'u') &&
                            tempChars[k].mark == Mark::None) {
                            tempChars[k].mark = Mark::Horn;
                            handled = true;
                            break;
                        }
                    }
                    break;
                }
                case '8': {
                    // Breve: a→ă
                    for (int k = static_cast<int>(tempChars.size()) - 1;
                         k >= 0; --k) {
                        if (std::tolower(tempChars[k].base) == 'a' &&
                            tempChars[k].mark == Mark::None) {
                            tempChars[k].mark = Mark::Breve;
                            handled = true;
                            break;
                        }
                    }
                    break;
                }
                case '9': {
                    // Stroke: d→đ
                    for (int k = static_cast<int>(tempChars.size()) - 1;
                         k >= 0; --k) {
                        if (std::tolower(tempChars[k].base) == 'd' &&
                            tempChars[k].mark == Mark::None) {
                            tempChars[k].mark = Mark::Stroke;
                            handled = true;
                            break;
                        }
                    }
                    break;
                }
                case '0': {
                    // Remove all marks and tones
                    for (auto &tc : tempChars) {
                        tc.mark = Mark::None;
                        tc.tone = Tone::None;
                    }
                    handled = true;
                    break;
                }
                }
                if (handled) continue;
            }
        }

        // Regular character: add to chars
        VChar vc;
        vc.base = lower;
        vc.upper = upper;
        tempChars.push_back(vc);
    }

    chars_ = tempChars;
    parseSyllable();
}

// ============================================================================
// Syllable parsing
// ============================================================================

void VietnameseEngine::parseSyllable() {
    vowelStart_ = vowelEnd_ = consonantEnd_ = -1;

    if (chars_.empty()) return;

    // Find first vowel
    for (int i = 0; i < static_cast<int>(chars_.size()); ++i) {
        if (chars_[i].isVowel()) {
            vowelStart_ = i;
            break;
        }
    }

    if (vowelStart_ < 0) return; // No vowels found

    // Find last vowel in the vowel cluster
    vowelEnd_ = vowelStart_;
    for (int i = vowelStart_ + 1; i < static_cast<int>(chars_.size()); ++i) {
        if (chars_[i].isVowel()) {
            vowelEnd_ = i;
        } else {
            break;
        }
    }

    // Check for final consonants after the vowel cluster
    if (vowelEnd_ + 1 < static_cast<int>(chars_.size())) {
        consonantEnd_ = static_cast<int>(chars_.size()) - 1;
    }
}

// ============================================================================
// Tone placement
// ============================================================================

int VietnameseEngine::findToneTarget() const {
    if (vowelStart_ < 0) return -1;

    int vowelCount = vowelEnd_ - vowelStart_ + 1;

    // Single vowel: tone goes on it
    if (vowelCount == 1) {
        return vowelStart_;
    }

    // Check if any vowel already has a diacritical mark (â, ê, ô, ơ, ư, ă)
    // If so, the tone goes on that vowel
    for (int i = vowelStart_; i <= vowelEnd_; ++i) {
        if (chars_[i].mark != Mark::None) {
            return i;
        }
    }

    // Two vowels
    if (vowelCount == 2) {
        char v1 = std::tolower(chars_[vowelStart_].base);
        char v2 = std::tolower(chars_[vowelEnd_].base);

        // Special cases: oa, oe, uy → tone on second vowel
        if ((v1 == 'o' && v2 == 'a') || (v1 == 'o' && v2 == 'e') ||
            (v1 == 'u' && v2 == 'y')) {
            return vowelEnd_;
        }

        bool hasFinalConsonant = (consonantEnd_ > vowelEnd_);

        if (hasFinalConsonant) {
            // With final consonant: tone on second vowel
            return vowelEnd_;
        } else {
            // Without final consonant: tone on first vowel
            return vowelStart_;
        }
    }

    // Three or more vowels (triphthongs like iêu, ươi, uôi, etc.)
    if (vowelCount >= 3) {
        // Tone goes on the middle vowel
        return vowelStart_ + 1;
    }

    // Fallback
    return vowelStart_;
}

bool VietnameseEngine::applyTone(Tone tone) {
    int target = findToneTarget();
    if (target < 0 || target >= static_cast<int>(chars_.size())) {
        return false;
    }

    // Remove existing tone from all vowels
    for (auto &vc : chars_) {
        if (vc.isVowel()) {
            vc.tone = Tone::None;
        }
    }

    chars_[target].tone = tone;
    return true;
}

bool VietnameseEngine::applyMark(Mark mark, char target) {
    // Find the target character
    for (int i = static_cast<int>(chars_.size()) - 1; i >= 0; --i) {
        if (target != 0 && std::tolower(chars_[i].base) != std::tolower(target)) {
            continue;
        }
        if (chars_[i].canAcceptMark(mark)) {
            chars_[i].mark = mark;
            return true;
        }
    }
    return false;
}

bool VietnameseEngine::removeTone() {
    bool removed = false;
    for (auto &vc : chars_) {
        if (vc.isVowel() && vc.tone != Tone::None) {
            vc.tone = Tone::None;
            removed = true;
        }
    }
    return removed;
}

bool VietnameseEngine::removeMark() {
    bool removed = false;
    for (auto &vc : chars_) {
        if (vc.mark != Mark::None) {
            vc.mark = Mark::None;
            removed = true;
        }
    }
    return removed;
}

} // namespace skey
