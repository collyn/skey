#ifndef SKEY_SURROUNDING_CURSOR_H
#define SKEY_SURROUNDING_CURSOR_H
#include <cstddef>
#include <string>
#include <string_view>

// Per-input cursor index. Invalidate on surrounding-text events and local
// mirrored deletes; never use pointer/length alone to detect content changes.
class SurroundingCursor {
public:
    void invalidate() { valid_ = false; }
    size_t offset(const std::string &text, unsigned cursor) {
        if (!valid_ || cursor_ != cursor || size_ != text.size() || data_ != text.data()) {
            bytes_ = 0;
            for (unsigned left = cursor; left && bytes_ < text.size(); --left) {
                ++bytes_;
                while (bytes_ < text.size() &&
                       (static_cast<unsigned char>(text[bytes_]) & 0xc0) == 0x80) ++bytes_;
            }
            cursor_ = cursor;
            size_ = text.size();
            data_ = text.data();
            valid_ = true;
        }
        return bytes_;
    }
    bool endsWith(const std::string &text, unsigned cursor, std::string_view expected) {
        if (expected.empty() || text.size() < expected.size()) return false;
        const auto bytes = offset(text, cursor);
        return bytes >= expected.size() &&
               text.compare(bytes - expected.size(), expected.size(), expected.data(), expected.size()) == 0;
    }
private:
    bool valid_ = false;
    unsigned cursor_ = 0;
    size_t bytes_ = 0, size_ = 0;
    const char *data_ = nullptr;
};
#endif
