#include "charset.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

std::string latin1(std::initializer_list<uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        if (byte <= 0x7F) {
            out.push_back(static_cast<char>(byte));
        } else {
            out.push_back(static_cast<char>(0xC0 | (byte >> 6)));
            out.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
        }
    }
    return out;
}

bool expect(const char *name, const std::string &actual,
            const std::string &expected) {
    if (actual == expected) {
        std::cout << "PASS " << name << "\n";
        return true;
    }
    std::cerr << "FAIL " << name << "\n  expected bytes:";
    for (unsigned char byte : expected)
        std::cerr << ' ' << std::hex << static_cast<unsigned>(byte);
    std::cerr << "\n  actual bytes:";
    for (unsigned char byte : actual)
        std::cerr << ' ' << std::hex << static_cast<unsigned>(byte);
    std::cerr << std::dec << "\n";
    return false;
}

} // namespace

int main() {
    bool ok = true;
    ok &= expect("Unicode passthrough", skey::convertCharset("tiếng", skey::Charset::Unicode),
                 "tiếng");

    // The engine returns NFD as UTF-8.  The app must preserve the bytes,
    // rather than wrapping each byte as a Latin-1 character.
    ok &= expect("Unicode NFD", skey::convertCharset("ế", skey::Charset::UnicodeDecomposed),
                 "e\xCC\x82\xCC\x81");
    ok &= expect("Unicode NFD mixed text",
                 skey::convertCharset("ĐđưÍ", skey::Charset::UnicodeDecomposed),
                 "Đđu\xCC\x9BI\xCC\x81");

    // Legacy outputs are raw bytes and must be represented as Latin-1 UTF-8
    // for Fcitx commitText().  Check every non-Unicode engine charset so an
    // enum routing regression cannot silently fall back to Unicode.
    const std::string input = "ĐđưÍ";
    const struct {
        skey::Charset charset;
        const char *name;
        std::string expected;
    } legacy[] = {
        {skey::Charset::TCVN3, "TCVN3", latin1({0xA7, 0xAE, 0xAD, 0x90})},
        {skey::Charset::VNIWindows, "VNI Windows", latin1({0xD1, 0xF1, 0xF6, 0xCD})},
        {skey::Charset::WinCP1258, "Windows CP1258", latin1({0xD0, 0xF0, 0xFD, 0xCD})},
        {skey::Charset::VIQR, "VIQR", "DDddu+I'"},
        {skey::Charset::VPS, "VPS", latin1({0xF1, 0xC7, 0xDC, 0xB4})},
        {skey::Charset::VISCII, "VISCII", latin1({0xD0, 0xF0, 0xDF, 0xCD})},
        {skey::Charset::BKHCM1, "BKHCM1", latin1({0x7D, 0xBD, 0xF5, 0x8A})},
        {skey::Charset::VietwareF, "Vietware-F", latin1({0x98, 0xA2, 0xA7, 0xDB})},
        {skey::Charset::ISC, "ISC", latin1({0xA7, 0xAE, 0xAD, 0x90})},
        {skey::Charset::BKHCM2, "BKHCM2", latin1({0xC0, 0xE0, 0xFB, 0xD1})},
        {skey::Charset::VietwareX, "Vietware-X", latin1({0xC2, 0xE2, 0xE6, 0xCA})},
        {skey::Charset::VNIMac, "VNI-MAC", latin1({0x84, 0x96, 0x9A, 0xEA})},
    };
    for (const auto &test : legacy)
        ok &= expect(test.name, skey::convertCharset(input, test.charset), test.expected);

    return ok ? 0 : 1;
}
