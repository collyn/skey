#ifndef SKEY_UINPUT_DELETE_ACK_H
#define SKEY_UINPUT_DELETE_ACK_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace skey {
// An application surrounding-text update can acknowledge deletion, not commit
// acceptance. Retain a small guard after acknowledgement, and never re-delete
// or re-commit based on missing/stale snapshots.
class UinputDeleteAck {
public:
    static constexpr uint64_t guardUsec = 8000;
    static constexpr uint64_t timeoutUsec = 80000;
    static constexpr uint64_t pollUsec = 4000;

    void reset() { active_ = false; ackAt_ = 0; expected_.clear(); }
    bool start(const std::string &text, unsigned cursor, unsigned anchor,
               std::string_view oldWord, unsigned deletes, uint64_t now) {
        reset();
        if (text.size() > 65536 || oldWord.empty() || !deletes ||
            cursor != anchor || cursor < deletes) return false;
        const auto end = offset(text, cursor);
        const auto begin = offset(text, cursor - deletes);
        if (end == std::string::npos || begin == std::string::npos ||
            end < oldWord.size() || begin < end - oldWord.size() ||
            text.compare(end - oldWord.size(), oldWord.size(), oldWord) != 0)
            return false;
        expected_ = text;
        expected_.erase(begin, end - begin);
        cursor_ = cursor - deletes;
        startedAt_ = now;
        active_ = true;
        return true;
    }
    void observe(bool valid, const std::string &text, unsigned cursor,
                 unsigned anchor, uint64_t now) {
        if (!active_ || now <= startedAt_) return;
        if (valid && cursor == cursor_ && anchor == cursor_ && text == expected_) {
            if (!ackAt_ && now < startedAt_ + timeoutUsec) ackAt_ = now;
        } else {
            ackAt_ = 0; // a newer conflicting snapshot invalidates earlier evidence
        }
    }
    uint64_t remaining(uint64_t now) const {
        if (!active_) return 0;
        const auto deadline = startedAt_ + timeoutUsec;
        if (ackAt_) {
            const auto ready = ackAt_ + guardUsec;
            return now >= ready ? 0 : ready - now;
        }
        if (now >= deadline) return 0;
        return std::min(pollUsec, deadline - now);
    }
    // Event-driven frontends rearm on every surrounding update. Without an
    // acknowledgement, only the bounded fallback deadline needs a wakeup.
    uint64_t nextCheckDelay(uint64_t now) const {
        if (!active_) return 0;
        const auto ready = ackAt_ ? ackAt_ + guardUsec : startedAt_ + timeoutUsec;
        return now >= ready ? 0 : ready - now;
    }
    bool active() const { return active_; }
    bool acknowledged() const { return ackAt_ != 0; }
private:
    static size_t offset(const std::string &text, unsigned chars) {
        size_t at = 0;
        while (chars--) {
            if (at == text.size()) return std::string::npos;
            ++at;
            while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) ++at;
        }
        return at;
    }
    bool active_ = false;
    unsigned cursor_ = 0;
    uint64_t startedAt_ = 0, ackAt_ = 0;
    std::string expected_;
};
} // namespace skey
#endif
