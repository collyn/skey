#ifndef FCITX5_SKEY_A11Y_MONITOR_H
#define FCITX5_SKEY_A11Y_MONITOR_H

#include <atomic>
#include <chrono>
#include <cstdint>
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
    std::atomic<bool> focusMultiline_{false};
    std::atomic<bool> focusSingleLine_{false};
    std::atomic<uint64_t> focusSnapshotUsec_{0};
};

#endif // FCITX5_SKEY_A11Y_MONITOR_H
