#ifndef SKEY_GNOME_FOCUS_WATCHER_H
#define SKEY_GNOME_FOCUS_WATCHER_H

#include "gnome_app_identity.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct DBusConnection;
struct DBusMessage;

namespace skey::gnome {
/// Text of the installed .desktop file for a Shell app id, searched in the
/// XDG application dirs (plus snap/flatpak exports); bounded read.
std::optional<std::string> readInstalledDesktopFile(const std::string &id);
} // namespace skey::gnome

/// Names the app GNOME Shell has focused, for fcitx5 versions that report
/// every relayed Wayland client as "gnome-shell".
///
/// xdg-desktop-portal-gnome asks the shell for its running apps
/// (org.gnome.Shell.Introspect.GetRunningApplications) whenever they change,
/// focus included; the reply flags the focused app.  Calling Introspect
/// directly is refused to ordinary clients, so a second session-bus
/// connection becomes a passive monitor for exactly that call, its reply and
/// the shell's RunningApplicationsChanged signal.  The monitor connection
/// never sends anything after BecomeMonitor.  Only the focused id is kept.
///
/// Everything runs on a worker thread; the engine reads snapshot() and never
/// waits on the bus.
class GnomeFocusWatcher {
public:
  enum class State : uint8_t {
    Unavailable, ///< no shell/portal, monitor refused, or not started
    Pending,     ///< the shell announced a change; its reply has not arrived
    Unknown,     ///< no focused app with a stable id
    Known,       ///< `program` names the focused app
    Shell,       ///< the shell's own UI: overview search or lock screen
  };
  struct Snapshot {
    State state = State::Unavailable;
    std::string appId;   ///< Shell app id ("google-chrome.desktop")
    std::string program; ///< name for classification/config ("google-chrome")
    std::string note;    ///< why the state is what it is, for the log
    uint64_t generation = 0;
    uint64_t changedUsec = 0; ///< CLOCK_MONOTONIC
  };

  /// A Pending state with no reply by then becomes Unknown.
  static constexpr uint64_t kPendingTimeoutUsec = 400000;

  GnomeFocusWatcher();
  ~GnomeFocusWatcher();
  GnomeFocusWatcher(const GnomeFocusWatcher &) = delete;
  GnomeFocusWatcher &operator=(const GnomeFocusWatcher &) = delete;

  void start();
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }

  uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
  Snapshot snapshot() const;

  /// Test hook; production reads the XDG application dirs.
  void setDesktopFileReader(skey::gnome::DesktopFileReader reader);

  static const char *stateName(State state);

private:
  void threadFunc();
  bool resolveOwners();
  void openMonitor();
  void closeMonitor();
  void handleMonitorMessage(DBusMessage *message);
  void handleControlMessage(DBusMessage *message);
  void readShellFlags();
  void publish(uint64_t nowUsec);
  std::string programFor(const std::string &appId);

  std::thread thread_;
  std::atomic<bool> running_{false}, stopRequested_{false};
  std::atomic<uint64_t> generation_{0};
  int wakeFd_ = -1;

  mutable std::mutex mutex_;
  Snapshot published_;
  skey::gnome::DesktopFileReader reader_;

  // Worker-thread state.
  DBusConnection *control_ = nullptr, *monitor_ = nullptr;
  std::string shellOwner_, portalOwner_, note_;
  skey::gnome::IntrospectPairing pairing_;
  State base_ = State::Unavailable;
  std::string appId_;
  bool overview_ = false, locked_ = false;
  uint64_t pendingSinceUsec_ = 0, retryAtUsec_ = 0;
};

#endif
