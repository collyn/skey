#ifndef FCITX5_SKEY_CHARSET_H
#define FCITX5_SKEY_CHARSET_H

#include <string>

namespace skey {

/// Output character set / encoding.
enum class Charset { Unicode, TCVN3, VNIWindows };

/// Convert a UTF-8 Vietnamese string to the target charset.
///
/// For legacy single-byte charsets (TCVN3, VNI Windows), each output byte
/// is wrapped as the corresponding Latin-1 codepoint and encoded as UTF-8.
/// This ensures commitString() always receives valid UTF-8 while the
/// application, with the right font, renders the legacy charset correctly.
///
/// Unicode is returned unchanged.
std::string convertCharset(const std::string &utf8, Charset cs);

} // namespace skey

#endif // FCITX5_SKEY_CHARSET_H
