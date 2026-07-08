#ifndef FCITX5_SKEY_A11Y_MONITOR_H
#define FCITX5_SKEY_A11Y_MONITOR_H

#include <atomic>
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
    std::atomic<bool> debug_{false};
};

#endif // FCITX5_SKEY_A11Y_MONITOR_H
