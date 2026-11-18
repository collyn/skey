#include "gnome_focus_watcher.h"

#include <dbus/dbus.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char *kShellName = "org.gnome.Shell";
constexpr const char *kShellPath = "/org/gnome/Shell";
constexpr const char *kPortalName = "org.freedesktop.impl.portal.desktop.gnome";
constexpr const char *kScreenSaverName = "org.gnome.ScreenSaver";
constexpr const char *kScreenSaverPath = "/org/gnome/ScreenSaver";
constexpr const char *kIntrospectIface = "org.gnome.Shell.Introspect";
constexpr int kCallTimeoutMs = 1000;
constexpr uint64_t kMonitorRetryUsec = 10000000;
constexpr size_t kMaxDesktopFileBytes = 256 * 1024;

uint64_t monotonicUsec() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

// The unique owner of a well-known name, empty when there is none.
std::string nameOwner(DBusConnection *bus, const char *name) {
  DBusMessage *call = dbus_message_new_method_call(
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner");
  if (!call) return {};
  dbus_message_append_args(call, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
  DBusError error;
  dbus_error_init(&error);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, call, kCallTimeoutMs, &error);
  dbus_message_unref(call);
  std::string owner;
  if (reply) {
    const char *value = nullptr;
    if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID) && value)
      owner = value;
    dbus_message_unref(reply);
  }
  dbus_error_free(&error);
  return owner;
}

// Reads a boolean from a variant iterator (signature "v" containing "b").
bool variantBool(DBusMessageIter *variant, bool &out) {
  if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT) return false;
  DBusMessageIter inner;
  dbus_message_iter_recurse(variant, &inner);
  if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_BOOLEAN) return false;
  dbus_bool_t value = FALSE;
  dbus_message_iter_get_basic(&inner, &value);
  out = value;
  return true;
}

std::optional<bool> getBoolProperty(DBusConnection *bus, const char *service, const char *path,
                                    const char *iface, const char *property) {
  DBusMessage *call = dbus_message_new_method_call(service, path, "org.freedesktop.DBus.Properties", "Get");
  if (!call) return std::nullopt;
  dbus_message_append_args(call, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID);
  DBusError error;
  dbus_error_init(&error);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, call, kCallTimeoutMs, &error);
  dbus_message_unref(call);
  dbus_error_free(&error);
  if (!reply) return std::nullopt;
  std::optional<bool> result;
  DBusMessageIter it;
  bool value = false;
  if (dbus_message_iter_init(reply, &it) && variantBool(&it, value)) result = value;
  dbus_message_unref(reply);
  return result;
}

std::optional<bool> screenSaverActive(DBusConnection *bus) {
  DBusMessage *call = dbus_message_new_method_call(kScreenSaverName, kScreenSaverPath,
                                                   kScreenSaverName, "GetActive");
  if (!call) return std::nullopt;
  DBusError error;
  dbus_error_init(&error);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, call, kCallTimeoutMs, &error);
  dbus_message_unref(call);
  dbus_error_free(&error);
  if (!reply) return std::nullopt;
  dbus_bool_t active = FALSE;
  std::optional<bool> result;
  if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID))
    result = active != FALSE;
  dbus_message_unref(reply);
  return result;
}

// GetRunningApplications reply: a{sa{sv}}.  An app is focused when its
// "active-on-seats" property is a non-empty string array.
bool parseRunningApps(DBusMessage *reply, std::vector<skey::gnome::RunningApp> &apps) {
  DBusMessageIter top;
  if (!dbus_message_iter_init(reply, &top) || dbus_message_iter_get_arg_type(&top) != DBUS_TYPE_ARRAY)
    return false;
  DBusMessageIter dict;
  dbus_message_iter_recurse(&top, &dict);
  while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(&dict, &entry);
    skey::gnome::RunningApp app;
    if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
      const char *id = nullptr;
      dbus_message_iter_get_basic(&entry, &id);
      app.id = id ? id : "";
      dbus_message_iter_next(&entry);
      if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY) {
        DBusMessageIter props;
        dbus_message_iter_recurse(&entry, &props);
        while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
          DBusMessageIter prop;
          dbus_message_iter_recurse(&props, &prop);
          const char *key = nullptr;
          if (dbus_message_iter_get_arg_type(&prop) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&prop, &key);
          dbus_message_iter_next(&prop);
          if (key && std::string_view(key) == "active-on-seats" &&
              dbus_message_iter_get_arg_type(&prop) == DBUS_TYPE_VARIANT) {
            DBusMessageIter variant;
            dbus_message_iter_recurse(&prop, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY &&
                dbus_message_iter_get_element_type(&variant) == DBUS_TYPE_STRING) {
              DBusMessageIter seats;
              dbus_message_iter_recurse(&variant, &seats);
              app.focused = dbus_message_iter_get_arg_type(&seats) == DBUS_TYPE_STRING;
            }
          }
          dbus_message_iter_next(&props);
        }
      }
      apps.push_back(std::move(app));
    }
    dbus_message_iter_next(&dict);
  }
  return true;
}

bool addMatch(DBusConnection *bus, const std::string &rule) {
  DBusError error;
  dbus_error_init(&error);
  dbus_bus_add_match(bus, rule.c_str(), &error);
  const bool ok = !dbus_error_is_set(&error);
  dbus_error_free(&error);
  return ok;
}

void closeConnection(DBusConnection *&bus) {
  if (!bus) return;
  dbus_connection_close(bus);
  dbus_connection_unref(bus);
  bus = nullptr;
}

} // namespace

namespace skey::gnome {
std::optional<std::string> readInstalledDesktopFile(const std::string &id) {
  // Ids come from the shell; refuse anything that could leave the dirs.
  if (id.empty() || id.find('/') != std::string::npos || id.find("..") != std::string::npos)
    return std::nullopt;
  static const auto dirs = desktopFileDirs(
      std::getenv("XDG_DATA_HOME"), std::getenv("HOME"), std::getenv("XDG_DATA_DIRS"));
  for (const auto &dir : dirs) {
    std::ifstream file(dir + "/applications/" + id, std::ios::binary);
    if (!file.is_open()) continue;
    std::string text(kMaxDesktopFileBytes, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(file.gcount()));
    return text;
  }
  return std::nullopt;
}

} // namespace skey::gnome

GnomeFocusWatcher::GnomeFocusWatcher() : reader_(skey::gnome::readInstalledDesktopFile) {
  wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

GnomeFocusWatcher::~GnomeFocusWatcher() {
  stop();
  if (wakeFd_ >= 0) close(wakeFd_);
}

const char *GnomeFocusWatcher::stateName(State state) {
  switch (state) {
  case State::Unavailable: return "unavailable";
  case State::Pending: return "pending";
  case State::Unknown: return "unknown";
  case State::Known: return "known";
  case State::Shell: return "shell";
  }
  return "?";
}

void GnomeFocusWatcher::setDesktopFileReader(skey::gnome::DesktopFileReader reader) {
  std::lock_guard<std::mutex> lock(mutex_);
  reader_ = std::move(reader);
}

GnomeFocusWatcher::Snapshot GnomeFocusWatcher::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return published_;
}

void GnomeFocusWatcher::start() {
  if (running_.exchange(true, std::memory_order_acq_rel)) return;
  if (thread_.joinable()) thread_.join();
  stopRequested_.store(false);
  try {
    thread_ = std::thread(&GnomeFocusWatcher::threadFunc, this);
  } catch (...) {
    running_.store(false);
    throw;
  }
}

void GnomeFocusWatcher::stop() {
  stopRequested_.store(true);
  if (wakeFd_ >= 0) {
    uint64_t one = 1;
    while (write(wakeFd_, &one, sizeof(one)) < 0 && errno == EINTR) {}
  }
  if (thread_.joinable()) thread_.join();
  running_.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(mutex_);
  if (published_.state != State::Unavailable) {
    published_ = Snapshot{State::Unavailable, {}, {}, "stopped",
                          published_.generation + 1, monotonicUsec()};
    generation_.store(published_.generation, std::memory_order_release);
  }
}

std::string GnomeFocusWatcher::programFor(const std::string &appId) {
  static thread_local std::unordered_map<std::string, std::string> cache;
  if (auto it = cache.find(appId); it != cache.end()) return it->second;
  skey::gnome::DesktopFileReader reader;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    reader = reader_;
  }
  std::string program = skey::gnome::programForAppId(appId, reader);
  if (cache.size() >= 128) cache.clear();
  cache.emplace(appId, program);
  return program;
}

void GnomeFocusWatcher::publish(uint64_t nowUsec) {
  Snapshot next;
  next.note = note_;
  if (base_ == State::Unavailable) {
    next.state = State::Unavailable;
  } else if (locked_ || overview_) {
    next.state = State::Shell;
    next.program = "gnome-shell";
    next.note = locked_ ? "screen locked" : "overview";
  } else {
    next.state = base_;
    if (base_ == State::Known) {
      next.appId = appId_;
      next.program = programFor(appId_);
      if (next.program.empty()) next.state = State::Unknown;
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (next.state == published_.state && next.program == published_.program &&
      next.appId == published_.appId && next.note == published_.note)
    return;
  next.generation = published_.generation + 1;
  next.changedUsec = nowUsec;
  published_ = std::move(next);
  generation_.store(published_.generation, std::memory_order_release);
}

bool GnomeFocusWatcher::resolveOwners() {
  const std::string shell = nameOwner(control_, kShellName);
  const std::string portal = nameOwner(control_, kPortalName);
  const bool changed = shell != shellOwner_ || portal != portalOwner_;
  shellOwner_ = shell;
  portalOwner_ = portal;
  return changed;
}

void GnomeFocusWatcher::readShellFlags() {
  overview_ = getBoolProperty(control_, kShellName, kShellPath, kShellName, "OverviewActive").value_or(false);
  locked_ = screenSaverActive(control_).value_or(false);
}

void GnomeFocusWatcher::closeMonitor() {
  closeConnection(monitor_);
  pairing_.setOwners({}, {});
  pendingSinceUsec_ = 0;
  appId_.clear();
  base_ = State::Unavailable;
}

void GnomeFocusWatcher::openMonitor() {
  closeMonitor();
  if (shellOwner_.empty() || portalOwner_.empty()) {
    note_ = shellOwner_.empty() ? "no GNOME Shell on the session bus" : "no xdg-desktop-portal-gnome";
    return;
  }
  DBusError error;
  dbus_error_init(&error);
  monitor_ = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
  dbus_error_free(&error);
  if (!monitor_) {
    note_ = "cannot open the monitor connection";
    retryAtUsec_ = monotonicUsec() + kMonitorRetryUsec;
    return;
  }
  dbus_connection_set_exit_on_disconnect(monitor_, FALSE);

  const std::string rules[] = {
      "type='method_call',sender='" + portalOwner_ + "',interface='" + kIntrospectIface +
          "',member='GetRunningApplications'",
      "type='method_return',sender='" + shellOwner_ + "',destination='" + portalOwner_ + "'",
      "type='error',sender='" + shellOwner_ + "',destination='" + portalOwner_ + "'",
      // On the same connection as the replies so their order is kept.
      "type='signal',sender='" + shellOwner_ + "',interface='" + kIntrospectIface +
          "',member='RunningApplicationsChanged'",
  };
  std::vector<const char *> ruleArgs;
  for (const auto &rule : rules) ruleArgs.push_back(rule.c_str());
  DBusMessage *call = dbus_message_new_method_call(
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus.Monitoring", "BecomeMonitor");
  const char **ruleData = ruleArgs.data();
  dbus_uint32_t flags = 0;
  dbus_message_append_args(call, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &ruleData,
                           static_cast<int>(ruleArgs.size()), DBUS_TYPE_UINT32, &flags, DBUS_TYPE_INVALID);
  dbus_error_init(&error);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(monitor_, call, kCallTimeoutMs, &error);
  dbus_message_unref(call);
  if (!reply) {
    note_ = std::string("BecomeMonitor refused: ") + (error.message ? error.message : "no reply");
    dbus_error_free(&error);
    closeConnection(monitor_);
    retryAtUsec_ = monotonicUsec() + kMonitorRetryUsec;
    return;
  }
  dbus_message_unref(reply);
  dbus_error_free(&error);
  pairing_.setOwners(shellOwner_, portalOwner_);
  base_ = State::Unknown;
  retryAtUsec_ = 0;
  note_ = "watching the portal";
}

void GnomeFocusWatcher::handleMonitorMessage(DBusMessage *message) {
  const int type = dbus_message_get_type(message);
  const char *sender = dbus_message_get_sender(message);
  const char *destination = dbus_message_get_destination(message);
  const std::string from = sender ? sender : "";
  const std::string to = destination ? destination : "";
  const uint64_t now = monotonicUsec();
  if (type == DBUS_MESSAGE_TYPE_SIGNAL) {
    if (from == shellOwner_ &&
        dbus_message_is_signal(message, kIntrospectIface, "RunningApplicationsChanged")) {
      base_ = State::Pending;
      pendingSinceUsec_ = now;
      publish(now);
    }
    return;
  }
  if (type == DBUS_MESSAGE_TYPE_METHOD_CALL) {
    if (dbus_message_is_method_call(message, kIntrospectIface, "GetRunningApplications"))
      pairing_.noteCall(from, dbus_message_get_serial(message));
    return;
  }
  if (type != DBUS_MESSAGE_TYPE_METHOD_RETURN && type != DBUS_MESSAGE_TYPE_ERROR) return;
  if (!pairing_.takeReply(from, to, dbus_message_get_reply_serial(message))) return;
  std::vector<skey::gnome::RunningApp> apps;
  if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN &&
      std::string_view(dbus_message_get_signature(message)) == "a{sa{sv}}" &&
      parseRunningApps(message, apps)) {
    appId_ = skey::gnome::focusedAppId(apps);
    base_ = appId_.empty() ? State::Unknown : State::Known;
  } else {
    appId_.clear();
    base_ = State::Unknown;
  }
  pendingSinceUsec_ = 0;
  note_ = "watching the portal";
  publish(now);
}

void GnomeFocusWatcher::handleControlMessage(DBusMessage *message) {
  if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL) return;
  const uint64_t now = monotonicUsec();
  if (dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged")) {
    const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
    if (!dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &oldOwner,
                               DBUS_TYPE_STRING, &newOwner, DBUS_TYPE_INVALID) || !name)
      return;
    const std::string_view changed(name);
    if (changed == kScreenSaverName) {
      locked_ = screenSaverActive(control_).value_or(false);
    } else if (changed == kShellName || changed == kPortalName) {
      // A restarted shell or portal invalidates the last answer.
      if (resolveOwners()) {
        if (changed == kShellName) overview_ = false;
        openMonitor();
        if (changed == kShellName) readShellFlags();
      }
    }
    publish(now);
    return;
  }
  if (dbus_message_is_signal(message, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
    const char *sender = dbus_message_get_sender(message);
    if (!sender || shellOwner_ != sender) return;
    DBusMessageIter it;
    if (!dbus_message_iter_init(message, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
      return;
    const char *iface = nullptr;
    dbus_message_iter_get_basic(&it, &iface);
    if (!iface || std::string_view(iface) != kShellName) return;
    dbus_message_iter_next(&it);
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
      DBusMessageIter props;
      dbus_message_iter_recurse(&it, &props);
      while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter prop;
        dbus_message_iter_recurse(&props, &prop);
        const char *key = nullptr;
        if (dbus_message_iter_get_arg_type(&prop) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&prop, &key);
        dbus_message_iter_next(&prop);
        bool value = false;
        if (key && std::string_view(key) == "OverviewActive" && variantBool(&prop, value)) overview_ = value;
        dbus_message_iter_next(&props);
      }
    }
    dbus_message_iter_next(&it);
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
      DBusMessageIter names;
      dbus_message_iter_recurse(&it, &names);
      while (dbus_message_iter_get_arg_type(&names) == DBUS_TYPE_STRING) {
        const char *name = nullptr;
        dbus_message_iter_get_basic(&names, &name);
        if (name && std::string_view(name) == "OverviewActive")
          overview_ = getBoolProperty(control_, kShellName, kShellPath, kShellName, "OverviewActive").value_or(false);
        dbus_message_iter_next(&names);
      }
    }
    publish(now);
    return;
  }
  if (dbus_message_is_signal(message, kScreenSaverName, "ActiveChanged")) {
    dbus_bool_t active = FALSE;
    if (dbus_message_get_args(message, nullptr, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID)) {
      locked_ = active != FALSE;
      publish(now);
    }
  }
}

void GnomeFocusWatcher::threadFunc() {
  DBusError error;
  dbus_error_init(&error);
  control_ = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
  dbus_error_free(&error);
  if (!control_) {
    note_ = "no session bus";
    publish(monotonicUsec());
    return; // running_ stays true until stop(): start() must not respin
  }
  dbus_connection_set_exit_on_disconnect(control_, FALSE);
  for (const char *name : {kShellName, kPortalName, kScreenSaverName}) {
    addMatch(control_, std::string("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                                   "member='NameOwnerChanged',arg0='") + name + "'");
  }
  addMatch(control_, std::string("type='signal',sender='") + kShellName +
                         "',path='" + kShellPath + "',interface='org.freedesktop.DBus.Properties',"
                         "member='PropertiesChanged',arg0='" + kShellName + "'");
  addMatch(control_, std::string("type='signal',sender='") + kScreenSaverName + "',interface='" +
                         kScreenSaverName + "',member='ActiveChanged'");

  resolveOwners();
  openMonitor();
  if (!shellOwner_.empty()) readShellFlags();
  publish(monotonicUsec());

  while (!stopRequested_.load()) {
    const uint64_t now = monotonicUsec();
    int timeoutMs = -1;
    auto until = [&](uint64_t at) {
      const int ms = at <= now ? 0 : static_cast<int>(std::min<uint64_t>((at - now + 999) / 1000, 60000));
      timeoutMs = timeoutMs < 0 ? ms : std::min(timeoutMs, ms);
    };
    if (base_ == State::Pending && pendingSinceUsec_) until(pendingSinceUsec_ + kPendingTimeoutUsec);
    if (!monitor_ && retryAtUsec_) until(retryAtUsec_);
    if (dbus_connection_get_dispatch_status(control_) == DBUS_DISPATCH_DATA_REMAINS ||
        (monitor_ && dbus_connection_get_dispatch_status(monitor_) == DBUS_DISPATCH_DATA_REMAINS))
      timeoutMs = 0;

    int controlFd = -1, monitorFd = -1;
    dbus_connection_get_unix_fd(control_, &controlFd);
    if (monitor_) dbus_connection_get_unix_fd(monitor_, &monitorFd);
    pollfd fds[3];
    nfds_t count = 0;
    fds[count++] = {wakeFd_, POLLIN, 0};
    fds[count++] = {controlFd, static_cast<short>(POLLIN | (dbus_connection_get_outgoing_size(control_) ? POLLOUT : 0)), 0};
    if (monitorFd >= 0) fds[count++] = {monitorFd, POLLIN, 0};
    if (poll(fds, count, timeoutMs) < 0 && errno != EINTR) break;
    if (fds[0].revents & POLLIN) {
      uint64_t value;
      while (read(wakeFd_, &value, sizeof(value)) < 0 && errno == EINTR) {}
    }
    if (stopRequested_.load()) break;

    if (!dbus_connection_read_write(control_, 0)) {
      note_ = "session bus closed";
      closeMonitor();
      publish(monotonicUsec());
      break;
    }
    // Messages are popped, never dispatched: libdbus would answer method
    // calls on the monitor connection, and the bus drops monitors that send.
    for (int i = 0; i < 256; ++i) {
      DBusMessage *message = dbus_connection_pop_message(control_);
      if (!message) break;
      handleControlMessage(message);
      dbus_message_unref(message);
    }
    if (monitor_) {
      if (!dbus_connection_read_write(monitor_, 0)) {
        closeMonitor();
        note_ = "monitor connection dropped";
        retryAtUsec_ = monotonicUsec() + kMonitorRetryUsec;
        publish(monotonicUsec());
      } else {
        for (int i = 0; i < 256; ++i) {
          DBusMessage *message = dbus_connection_pop_message(monitor_);
          if (!message) break;
          handleMonitorMessage(message);
          dbus_message_unref(message);
        }
      }
    }

    const uint64_t after = monotonicUsec();
    if (base_ == State::Pending && pendingSinceUsec_ && after - pendingSinceUsec_ >= kPendingTimeoutUsec) {
      base_ = State::Unknown;
      appId_.clear();
      pendingSinceUsec_ = 0;
      note_ = "no portal reply after the shell's change signal";
      publish(after);
    }
    if (!monitor_ && retryAtUsec_ && after >= retryAtUsec_) {
      retryAtUsec_ = 0;
      resolveOwners();
      openMonitor();
      publish(after);
    }
  }

  closeMonitor();
  closeConnection(control_);
}
