#ifndef SKEY_A11Y_WORK_POLICY_H
#define SKEY_A11Y_WORK_POLICY_H
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

class A11yPokeCache {
public:
    bool due(const std::string &bus, const std::string &path, uint64_t now) const {
        const auto it = entries_.find(bus + '\n' + path);
        return it == entries_.end() || now >= it->second.next;
    }
    void result(const std::string &bus, const std::string &path, bool success, uint64_t now) {
        const auto key = bus + '\n' + path;
        if (!entries_.count(key) && entries_.size() >= 256) entries_.clear();
        auto &entry = entries_[key];
        if (success) {
            entry.failures = 0;
            // Keep the startup late retry: a stub may answer before its tree
            // becomes usable. Healthy apps then need only a recovery refresh.
            entry.next = now + (entry.warmed ? 60000000 : 2000000);
            entry.warmed = true;
        } else {
            entry.next = now + std::min<uint64_t>(1000000ULL << entry.failures, 60000000);
            entry.failures = std::min(entry.failures + 1, 6u);
        }
    }
    void removeBus(const std::string &bus) {
        const auto prefix = bus + '\n';
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) it = entries_.erase(it);
            else ++it;
        }
    }
private:
    struct Entry { uint64_t next = 0; unsigned failures = 0; bool warmed = false; };
    std::unordered_map<std::string, Entry> entries_;
};

// Parent + parent role of nodes the focus ancestor walk already resolved.
// Each walk level costs two blocking round trips (Parent, GetRole) into the
// focused app — 10-44 levels in web pages; focus moving inside the same
// page or window meets known ancestors after a level or two.  Toolkits hand
// out a fresh object path per node, so an entry stays right while its node
// lives; the TTL bounds path reuse after destruction, and an app leaving
// the bus drops its entries.
class A11yAncestorCache {
public:
    struct Entry { std::string parentBus, parentPath; int parentRole = -1; uint64_t at = 0; };
    static constexpr uint64_t kTtlUsec = 30000000; // 30s
    const Entry *find(const std::string &bus, const std::string &path, uint64_t now) const {
        const auto it = entries_.find(bus + '\n' + path);
        if (it == entries_.end() || now - it->second.at >= kTtlUsec) return nullptr;
        return &it->second;
    }
    void put(const std::string &bus, const std::string &path, Entry entry) {
        if (entries_.size() >= 4096) entries_.clear();
        entries_[bus + '\n' + path] = std::move(entry);
    }
    void removeBus(const std::string &bus) {
        const auto prefix = bus + '\n';
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) it = entries_.erase(it);
            else ++it;
        }
    }
private:
    std::unordered_map<std::string, Entry> entries_;
};
#endif
