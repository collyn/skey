/**
 * viet_util.h — Pure inline helpers for Vietnamese text processing.
 *
 * Shared by VietnameseEngine methods.  All functions are stateless and
 * live in namespace skey::detail.
 */

#ifndef FCITX5_SKEY_VIET_UTIL_H
#define FCITX5_SKEY_VIET_UTIL_H

#include <string>

namespace skey {
namespace detail {

/// Case-fold a single ASCII character.  'A'-'Z' → 'a'-'z'; all others unchanged.
inline char toLowerASCII(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

/// True when `c` (case-insensitive) is one of the six Vietnamese vowel letters.
inline bool isVowel(char c) {
    char lc = toLowerASCII(c);
    return lc == 'a' || lc == 'e' || lc == 'i' || lc == 'o' || lc == 'u' || lc == 'y';
}

/// True when `c` (case-insensitive) is a Telex tone key (s/f/r/x/j/z).
inline bool isToneKey(char c) {
    char lc = toLowerASCII(c);
    return lc == 's' || lc == 'f' || lc == 'r' || lc == 'x' || lc == 'j' || lc == 'z';
}

/// True when every byte in `s` is < 128 (no Vietnamese / multi-byte UTF-8).
inline bool isAllAscii(const std::string &s) {
    for (unsigned char ch : s) {
        if (ch > 127) return false;
    }
    return true;
}

/// First vowel position in a Telex raw string.  Returns std::string::npos if
/// no vowel is found.
///
/// Tone-key characters BEFORE the first vowel are regular letters (e.g. 'x'
/// in "xin"), not tone marks.  Both dedup (recompose R3) and double-tone
/// undo (processKey P5) rely on this boundary.
inline size_t findFirstVowel(const std::string &s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (isVowel(s[i])) return i;
    }
    return std::string::npos;
}

/**
 * Deduplicate repeated tone keys in a Telex raw input string.
 *
 * Edge case: bamboo-core fails to compose "looixfsx" — x…x with f,s
 * between.  In Telex each new tone REPLACES the previous, so only the
 * last occurrence matters.
 *
 * Consecutive same-tone pairs ("xx", "ss") are PRESERVED — they are
 * intentional undo patterns handled by tryToneKeyUndo().
 *
 * @param input      Raw Telex input string.
 * @param firstVowel Index of first vowel (from findFirstVowel).
 * @return Deduplicated string with redundant non-consecutive tone keys
 *         removed.
 *
 * Bamboo limitation: bamboo-core mis-handles non-consecutive repeated
 * tone keys — this is the strongest upstream candidate.
 */
inline std::string deduplicateToneKeys(const std::string &input,
                                       size_t firstVowel) {
    std::string result;
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (isToneKey(c) &&
            (firstVowel == std::string::npos || i > firstVowel)) {
            // Check whether a later occurrence of the SAME tone key exists
            // with a DIFFERENT tone key between them.
            bool later = false, diffBetween = false;
            for (size_t j = i + 1; j < input.size(); ++j) {
                char cj = input[j];
                char cl = toLowerASCII(c);
                char cjl = toLowerASCII(cj);
                if (cl == cjl) { later = true; break; }
                if (isToneKey(cj)) diffBetween = true;
            }
            if (later && diffBetween) continue;  // skip this earlier occurrence
        }
        result += c;
    }
    return result;
}

/// True when `s` contains any ASCII vowel (a, e, i, o, u, y, case-insensitive).
/// Used by ddFreeStyle: words without vowels that contain đ are abbreviations.
inline bool hasAsciiVowel(const std::string &s) {
    for (unsigned char c : s) {
        if (c > 127) continue;  // skip multi-byte UTF-8 continuation bytes
        char lc = toLowerASCII(static_cast<char>(c));
        if (lc == 'a' || lc == 'e' || lc == 'i' ||
            lc == 'o' || lc == 'u' || lc == 'y')
            return true;
    }
    return false;
}

/// True when `s` contains 'đ' (U+0111, \xC4\x91) or 'Đ' (U+0110, \xC4\x90).
inline bool containsD(const std::string &s) {
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) == 0xC4) &&
            (static_cast<unsigned char>(s[i + 1]) == 0x91 ||
             static_cast<unsigned char>(s[i + 1]) == 0x90))
            return true;
    }
    return false;
}

} // namespace detail
} // namespace skey

#endif // FCITX5_SKEY_VIET_UTIL_H
