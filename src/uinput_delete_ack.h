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
    // A snapshot delivered this long after the backspaces were sent is
    // fresh — not a pre-delete echo still in flight.
    static constexpr uint64_t missMinAgeUsec = 12000;

    void reset() {
        active_ = false;
        ackAt_ = 0;
        ackOnce_ = false;
        missed_ = false;
        expected_.clear();
        word_.clear();
    }
    // The deletion targets the word's tail ending before the caret.  The
    // caret/anchor may be a selection (browser autofill) — the expected
    // post-delete state keeps the selection shifted, and the old word must
    // end right before the selection start.
    bool start(const std::string &text, unsigned cursor, unsigned anchor,
               std::string_view oldWord, unsigned deletes, uint64_t now,
               uint64_t deadlineUsec = timeoutUsec) {
        reset();
        if (text.size() > 65536 || oldWord.empty() || !deletes) return false;
        const unsigned selStart = std::min(cursor, anchor);
        const unsigned selEnd = std::max(cursor, anchor);
        if (selStart < deletes) return false;
        const auto selBeginB = offset(text, selStart);
        const auto selEndB = offset(text, selEnd);
        const auto deleteBeginB = offset(text, selStart - deletes);
        if (selBeginB == std::string::npos || selEndB == std::string::npos ||
            deleteBeginB == std::string::npos ||
            selBeginB < oldWord.size() ||
            text.compare(selBeginB - oldWord.size(), oldWord.size(), oldWord) != 0)
            return false;
        expected_ = text;
        expected_.erase(deleteBeginB, selBeginB - deleteBeginB);
        cursor_ = selStart - deletes;
        expectedAnchor_ = selEnd - deletes;
        word_ = oldWord;
        startedAt_ = now;
        deadlineUsec_ = deadlineUsec;
        active_ = true;
        return true;
    }
    void observe(bool valid, const std::string &text, unsigned cursor,
                 unsigned anchor, uint64_t now) {
        if (!active_ || now <= startedAt_) return;
        if (valid && cursor == cursor_ && anchor == expectedAnchor_ &&
            text == expected_) {
            if (!ackAt_ && now < startedAt_ + deadlineUsec_) {
                ackAt_ = now;
                ackOnce_ = true;
            }
        } else {
            ackAt_ = 0; // a newer conflicting snapshot invalidates earlier evidence
            // A late pre-BS echo can re-deliver a snapshot containing the
            // word, so a containing one is only miss evidence before any
            // acknowledgement — once the deletion was confirmed, it stays
            // confirmed.
            if (!missed_ && !ackOnce_ && valid &&
                now >= startedAt_ + missMinAgeUsec &&
                text.find(word_) != std::string::npos) {
                missed_ = true;
            }
        }
    }
    uint64_t remaining(uint64_t now) const {
        if (!active_) return 0;
        const auto deadline = startedAt_ + deadlineUsec_;
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
        const auto ready = ackAt_ ? ackAt_ + guardUsec : startedAt_ + deadlineUsec_;
        return now >= ready ? 0 : ready - now;
    }
    bool active() const { return active_; }
    bool acknowledged() const { return ackAt_ != 0; }
    bool missed() const { return missed_; }
    uint64_t observedLatencyUsec() const {
        return ackAt_ ? ackAt_ - startedAt_ : 0;
    }
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
    bool ackOnce_ = false;
    bool missed_ = false;
    unsigned cursor_ = 0;
    unsigned expectedAnchor_ = 0;
    uint64_t startedAt_ = 0, ackAt_ = 0, deadlineUsec_ = timeoutUsec;
    std::string expected_, word_;
};
} // namespace skey
#endif
