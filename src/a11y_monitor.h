#ifndef FCITX5_SKEY_A11Y_MONITOR_H
#define FCITX5_SKEY_A11Y_MONITOR_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

/// Monitors AT-SPI2 accessibility focus events to detect whether
/// the currently focused element is a browser address bar or web content.
/// Runs a background thread that listens for D-Bus signals from the
/// AT-SPI2 bus and queries the focused element's role/ancestors.
class A11yMonitor {
public:
    A11yMonitor();
    ~A11yMonitor();

    A11yMonitor(const A11yMonitor &) = delete;
    A11yMonitor &operator=(const A11yMonitor &) = delete;

    /// Start monitoring. Safe to call multiple times (no-op if running).
    void start();
    /// Stop monitoring.
    void stop();

    /// Returns true if the currently focused element is a browser UI element
    /// (address bar, search bar, etc.) — i.e. NOT inside a web document.
    bool isBrowserUIFocused() const {
        return browserUIFocused_.load(std::memory_order_relaxed);
    }

    /// Returns true if the currently focused element is inside a web
    /// document (inverse of isBrowserUIFocused).
    bool isWebContentFocused() const {
        return focusInWebDoc_.load(std::memory_order_relaxed);
    }

    /// Role of the last focused element (ATSPI_ROLE_* values).
    int focusRole() const {
        return focusRole_.load(std::memory_order_relaxed);
    }

    /// True when the last focus snapshot is at most `maxAgeUsec` old
    /// (CLOCK_MONOTONIC microseconds).
    bool isFocusSnapshotFresh(uint64_t maxAgeUsec) const {
        uint64_t snap = focusSnapshotUsec_.load(std::memory_order_relaxed);
        if (snap == 0)
            return false;
        uint64_t nowUsec = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() /
            1000);
        return nowUsec - snap <= maxAgeUsec;
    }

    /// Returns true if the currently focused element is a password field.
    bool isPasswordFocused() const {
        return passwordFocused_.load(std::memory_order_relaxed);
    }

    /// PID of the process serving the last focused accessible (-1 when
    /// unknown).  Captured on the monitor thread during focus events so
    /// the engine can run pid-targeted /proc checks instead of full scans.
    int focusProcessId() const {
        return focusProcessId_.load(std::memory_order_relaxed);
    }

    /// True when the last focus snapshot was a real text-entry element
    /// (role TEXT / ENTRY / DOCUMENT_TEXT).  A Chromium tab whose focus is
    /// NOT a text entry (clicking a Google Sheets cell focuses the
    /// document/combo box while caps still carry the previous editor's
    /// hints) cannot receive surrounding-text replacements — the engine
    /// routes those to Uinput.  Same freshness window as the focus
    /// snapshot: both are written in the same focus-event handler.
    bool isTextEntryFocused() const {
        return textEntryFocused_.load(std::memory_order_relaxed);
    }

    /// ATSPI_STATE_EDITABLE of the last focused element.  Chrome (>=150)
    /// reports editable=0 for several real inputs (Facebook chat
    /// textarea), so it cannot be the sole discriminator.
    bool isFocusEditable() const {
        return focusEditable_.load(std::memory_order_relaxed);
    }

    /// Line-state of the last focused element.  Real text inputs carry at
    /// least one of single-line / multi-line; the Google Sheets cell
    /// editor reports neither (role ENTRY, editable=0, no line state).
    bool isFocusSingleLine() const {
        return focusSingleLine_.load(std::memory_order_relaxed);
    }

    bool isFocusMultiline() const {
        return focusMultiline_.load(std::memory_order_relaxed);
    }

    /// Identity of the last focused text-entry element (bus name + object
    /// path) for the engine's own AT-SPI2 queries (GetText/GetSelection).
    /// Returns false when no text entry has been focused yet.
    bool focusedTextEntry(std::string &busName, std::string &path,
                          uint64_t &snapshotUsec) const;

    /// Background snapshot of the focused entry's text + selection,
    /// polled by the monitor thread.  Returns true when the snapshot is
    /// at most `maxAgeUsec` old.
    bool a11yState(std::string &text, int &selStart, int &selEnd,
                   uint64_t maxAgeUsec) const;

    /// Enable/disable the snapshot polling.  The engine enables it only
    /// while the current input context is the Chromium address bar on
    /// X11 — polling any other focused entry is wasted DBus traffic.
    void setPollingEnabled(bool enabled) {
        pollEnabled_.store(enabled, std::memory_order_relaxed);
    }

    /// Block until the snapshot is updated (or `timeoutUsec` elapses).
    /// Used by the engine's replacement path instead of usleep polling.
    void waitForSnapshotUpdate(uint64_t timeoutUsec) const;

    /// AT-SPI2 bus address (from the X11 root property or session bus).
    /// Empty when unavailable.
    static std::string atspiBusAddress();

    /// Returns true if the monitor is connected and running.
    bool isRunning() const {
        return running_.load(std::memory_order_relaxed);
    }

    /// Enable/disable debug logging to /tmp/skey_a11y.log
    void setDebug(bool enabled) {
        debug_.store(enabled, std::memory_order_relaxed);
    }

private:
    void threadFunc();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> browserUIFocused_{false};
    std::atomic<bool> passwordFocused_{false};
    std::atomic<bool> debug_{false};
    // Snapshot of the last focus event (see getters above).
    std::atomic<bool> focusInWebDoc_{false};
    std::atomic<int> focusRole_{0};
    std::atomic<bool> focusEditable_{false};
    std::atomic<int> focusProcessId_{-1};
    std::atomic<bool> focusMultiline_{false};
    std::atomic<bool> focusSingleLine_{false};
    std::atomic<bool> textEntryFocused_{false};
    std::atomic<uint64_t> focusSnapshotUsec_{0};
    // Focused text-entry identity (guarded — strings are not atomic).
    mutable std::mutex focusEntryMutex_;
    std::string focusEntryBus_;
    std::string focusEntryPath_;
    std::atomic<uint64_t> focusEntrySnapshotUsec_{0};
    // Polled text + selection snapshot (guarded — strings are not atomic).
    mutable std::mutex a11ySnapshotMutex_;
    std::string a11ySnapshotText_;
    int a11ySnapshotSelStart_ = -1;
    int a11ySnapshotSelEnd_ = -1;
    uint64_t a11ySnapshotUsec_ = 0;
    uint64_t lastA11yPollUsec_ = 0;
    bool a11yPollDirty_ = false; // monitor-thread only
    std::atomic<bool> pollEnabled_{false};
    mutable std::condition_variable snapshotCv_;
};

#endif // FCITX5_SKEY_A11Y_MONITOR_H
