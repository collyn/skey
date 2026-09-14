#ifndef SKEY_SHEETS_CELL_TRACKER_H
#define SKEY_SHEETS_CELL_TRACKER_H

#include <string>

// Google Sheets reuses waffle-rich-text-editor across cells. Its accessible
// name is the A1 address; text changes and empty names are not cell changes.
// Used only on the accessibility thread.
class SheetsCellTracker {
public:
    void focus(const std::string &bus, const std::string &path,
               bool isSheetsEditor, const std::string &name) {
        if (!isSheetsEditor) {
            bus_.clear();
            path_.clear();
            cell_.clear();
        } else if (bus_ != bus || path_ != path) {
            bus_ = bus;
            path_ = path;
            cell_ = isCellAddress(name) ? name : std::string();
        }
    }

    bool update(const std::string &bus, const std::string &path,
                const std::string &name) {
        if (path_.empty() || bus != bus_ || path != path_ ||
            !isCellAddress(name))
            return false;
        const bool changed = !cell_.empty() && cell_ != name;
        cell_ = name;
        return changed;
    }

    // Shared with the synchronous reader: only a validated cell address may
    // advance its baseline; transient empty names must not erase it.
    static bool isCellAddress(const std::string &name) {
        size_t i = 0;
        while (i < name.size() && name[i] >= 'A' && name[i] <= 'Z')
            ++i;
        if (i == 0 || i > 3 || i == name.size() ||
            name[i] < '1' || name[i] > '9')
            return false;
        for (; i < name.size(); ++i)
            if (name[i] < '0' || name[i] > '9')
                return false;
        return true;
    }

private:
    std::string bus_, path_, cell_;
};

// Key-thread baseline, intentionally separate from the async event tracker.
// Delayed events must never overwrite a newer synchronous observation.
class SheetsCellSnapshot {
public:
    bool observe(const std::string &identity, const std::string &cell) {
        if (!SheetsCellTracker::isCellAddress(cell)) return false;
        const bool changed = !cell_.empty() &&
            (identity_ != identity || cell_ != cell);
        identity_ = identity;
        cell_ = cell;
        return changed;
    }
private:
    std::string identity_, cell_;
};

#endif
