#ifndef SKEY_A11Y_TEXT_CACHE_H
#define SKEY_A11Y_TEXT_CACHE_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// Text snapshots are demand-driven and belong to one focus/polling generation.
// The monitor performs IPC outside the lock; late replies cannot resurrect a
// snapshot after focus loss or after the engine disables polling.
class A11yTextCache {
public:
    static constexpr uint64_t idleUsec = 500000;
    static constexpr uint64_t minPollUsec = 15000;
    static constexpr uint64_t pollUsec = 150000;

    struct Request {
        std::string bus, path;
        uint64_t generation, started;
    };

    // Returns true when the monitor must wake to update subscriptions/poll.
    bool setEnabled(bool enabled, uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool wake = enabled != enabled_ || (enabled && !activeLocked(now));
        if (wake) {
            invalidateLocked();
            dirty_ = true;
            lastPoll_ = retryAt_ = 0;
            failures_ = 0;
        }
        enabled_ = enabled;
        demandUsec_ = now;
        return wake;
    }

    void focus(const std::string &bus, const std::string &path, uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bus_ != bus || path_ != path) {
            invalidateLocked();
            bus_ = bus;
            path_ = path;
            dirty_ = true;
            lastPoll_ = retryAt_ = 0;
            failures_ = 0;
        }
        focusUsec_ = now;
    }

    bool focusedEntry(std::string &bus, std::string &path, uint64_t &stamp) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path_.empty()) return false;
        bus = bus_;
        path = path_;
        stamp = focusUsec_;
        return true;
    }

    void changed(const char *bus, const char *path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bus && path && !path_.empty() && bus_ == bus && path_ == path)
            dirty_ = true;
    }

    void blur(const char *bus, const char *path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bus && path && !path_.empty() && bus_ == bus && path_ == path) {
            invalidateLocked();
            bus_.clear();
            path_.clear();
        }
    }

    bool active(uint64_t now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return activeLocked(now) && !path_.empty();
    }

    int waitMillis(uint64_t now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!activeLocked(now) || path_.empty()) return 200;
        uint64_t due = lastPoll_ ? lastPoll_ + (dirty_ ? minPollUsec : pollUsec) : now;
        due = std::max(due, retryAt_);
        // Wake on lease expiry too, to remove idle text subscriptions.
        due = std::min(due, demandUsec_ + idleUsec);
        return due <= now ? 0 : static_cast<int>(
            std::min<uint64_t>((due - now + 999) / 1000, 200));
    }

    std::optional<Request> beginPoll(uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!activeLocked(now) || path_.empty() || now < retryAt_)
            return std::nullopt;
        if (lastPoll_ && (now - lastPoll_ < minPollUsec ||
                         (!dirty_ && now - lastPoll_ < pollUsec)))
            return std::nullopt;
        dirty_ = false;
        lastPoll_ = now;
        return Request{bus_, path_, generation_, now};
    }

    bool finishPoll(const Request &request, bool ok, std::string text,
                    int start, int end, uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.generation != generation_ || !activeLocked(now))
            return false;
        if (!ok) {
            text_.clear();
            snapshotUsec_ = 0;
            start_ = end_ = -1;
            // Text events must not defeat backoff against an unresponsive app.
            retryAt_ = now + std::min<uint64_t>(pollUsec << failures_, 1000000);
            failures_ = std::min(failures_ + 1, 3u);
        } else {
            text_ = std::move(text);
            start_ = start;
            end_ = end;
            // Date from request start: a slow reply is not a fresh snapshot.
            snapshotUsec_ = request.started;
            failures_ = 0;
            retryAt_ = 0;
        }
        cv_.notify_all();
        return ok;
    }

    bool read(std::string &text, int &start, int &end,
              uint64_t now, uint64_t maxAge) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!activeLocked(now) || !snapshotUsec_ || now < snapshotUsec_ ||
            now - snapshotUsec_ > maxAge) {
            text.clear();
            start = end = -1;
            return false;
        }
        text = text_;
        start = start_;
        end = end_;
        return true;
    }

    uint64_t stamp() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshotUsec_;
    }

    void waitForUpdate(uint64_t observed, uint64_t timeoutUsec) const {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto generation = generation_;
        cv_.wait_for(lock, std::chrono::microseconds(timeoutUsec), [&] {
            return snapshotUsec_ != observed || generation_ != generation;
        });
    }

private:
    bool activeLocked(uint64_t now) const {
        return enabled_ && now >= demandUsec_ && now - demandUsec_ < idleUsec;
    }

    void invalidateLocked() {
        ++generation_;
        text_.clear();
        snapshotUsec_ = 0;
        start_ = end_ = -1;
        cv_.notify_all();
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::string bus_, path_, text_;
    uint64_t generation_ = 0, focusUsec_ = 0, snapshotUsec_ = 0;
    uint64_t demandUsec_ = 0, lastPoll_ = 0, retryAt_ = 0;
    unsigned failures_ = 0;
    int start_ = -1, end_ = -1;
    bool enabled_ = false, dirty_ = false;
};

#endif
