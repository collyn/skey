#include "charset.h"

#include "skey_engine.h"

#include <cstring>
#include <string>

namespace skey {

// =============================================================================
// Charset conversion — delegates to skey-engine's Rust charset module.
//
// skey-engine has a complete 213-character pivot table (ported from x-unikey)
// covering all Vietnamese characters across TCVN3, VNI-WIN, WinCP1258, and
// VIQR.  The C++ side is a thin wrapper that calls the Rust FFI and wraps
// each output byte as a Latin-1 UTF-8 codepoint so that commitString()
// always receives valid UTF-8.
// =============================================================================

/// Encode a byte as its Latin-1 UTF-8 representation.
static void appendByte(std::string &result, uint8_t b) {
    if (b <= 0x7F) {
        result += static_cast<char>(b);
    } else {
        result += static_cast<char>(0xC0 | (b >> 6));
        result += static_cast<char>(0x80 | (b & 0x3F));
    }
}

std::string convertCharset(const std::string &utf8, Charset cs) {
    if (cs == Charset::Unicode) return utf8;
    if (utf8.empty()) return utf8;

    // C++ Charset and Rust VietCharset enums share the same order
    int csId = static_cast<int>(cs);

    size_t outLen = 0;
    uint8_t *raw = skey_charset_encode(utf8.c_str(), csId, &outLen);
    if (!raw) return utf8;

    std::string result;
    result.reserve(outLen * 2);
    for (size_t i = 0; i < outLen; i++) {
        appendByte(result, raw[i]);
    }

    skey_charset_free_buf(raw);
    return result;
}

} // namespace skey
