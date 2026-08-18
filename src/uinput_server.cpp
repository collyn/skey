#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <pwd.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>

namespace {

// Timing tunables — adjust if input is lost or laggy
constexpr useconds_t UINPUT_INIT_WAIT_US = 1000000;
// Backspaces per write() batch.  Batching press+SYN+release+SYN per key
// into single write()s with a 1ms poll() pace between batches cuts
// deletion latency ~4x versus the old per-event usleep pacing (300µs per
// key event + 400µs per BS) while the gap keeps laggy apps (Telegram,
// X11) from dropping events.
constexpr int BACKSPACE_BATCH_SIZE = 4;
constexpr int BATCH_GAP_MS = 1;

std::atomic<bool> running{true};

void onSignal(int) { running.store(false); }

class Fd {
public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd() { reset(); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  int get() const { return fd_; }
  int release() {
    int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void reset(int fd = -1) {
    if (fd_ >= 0) {
      close(fd_);
    }
    fd_ = fd;
  }
  explicit operator bool() const { return fd_ >= 0; }

private:
  int fd_;
};

std::string socketPath(const std::string &username) {
  std::string path = "skeysocket-" + username + "-kb_socket";
  constexpr size_t maxAbstractSocketName =
      sizeof(((sockaddr_un *)0)->sun_path) - 1;
  if (path.size() > maxAbstractSocketName) {
    path.resize(maxAbstractSocketName);
  }
  return path;
}

passwd *lookupUser(const std::string &name, std::vector<char> &buf,
                   passwd &pwd) {
  passwd *result = nullptr;
  long bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (bufSize < 0) {
    bufSize = 16384;
  }
  buf.resize(static_cast<size_t>(bufSize));
  if (getpwnam_r(name.c_str(), &pwd, buf.data(), buf.size(), &result) != 0) {
    return nullptr;
  }
  return result;
}

std::string currentUsername() {
  passwd pwd{};
  passwd *result = nullptr;
  long bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (bufSize < 0) {
    bufSize = 16384;
  }
  std::vector<char> buf(static_cast<size_t>(bufSize));
  if (getpwuid_r(getuid(), &pwd, buf.data(), buf.size(), &result) == 0 &&
      result) {
    return result->pw_name;
  }
  return "unknown";
}

bool executableIsFcitx5(pid_t pid) {
  char procPath[64];
  char exePath[4096];
  snprintf(procPath, sizeof(procPath), "/proc/%d/exe", pid);
  ssize_t len = readlink(procPath, exePath, sizeof(exePath) - 1);
  if (len < 0) {
    return false;
  }
  exePath[len] = '\0';
  std::string path(exePath);
  return path == "/usr/bin/fcitx5" ||
         (path.size() >= 7 && path.compare(path.size() - 7, 7, "/fcitx5") == 0);
}

class UinputDevice {
public:
  ~UinputDevice() {
    if (fd_) {
      ioctl(fd_.get(), UI_DEV_DESTROY);
    }
  }

  bool init() {
    fd_.reset(open("/dev/uinput", O_WRONLY | O_NONBLOCK));
    if (!fd_) {
      std::cerr << "open /dev/uinput failed: " << strerror(errno) << "\n";
      return false;
    }
    if (ioctl(fd_.get(), UI_SET_EVBIT, EV_KEY) < 0) {
      std::cerr << "configure uinput failed: " << strerror(errno) << "\n";
      return false;
    }
    const int keys[] = {KEY_BACKSPACE, KEY_ESC};
    for (int key : keys) {
      if (ioctl(fd_.get(), UI_SET_KEYBIT, key) < 0) {
        std::cerr << "configure key failed: " << strerror(errno) << "\n";
        return false;
      }
    }

    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x534b;
    setup.id.product = 0x0001;
    strncpy(setup.name, "SKey-Uinput-Server", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd_.get(), UI_DEV_SETUP, &setup) < 0 ||
        ioctl(fd_.get(), UI_DEV_CREATE) < 0) {
      std::cerr << "create uinput device failed: " << strerror(errno) << "\n";
      return false;
    }
    usleep(UINPUT_INIT_WAIT_US);
    return true;
  }

  // press+SYN+release+SYN for one key, sent in a single write().
  void tap(int code) const {
    input_event ev[4]{};
    ev[0].type = EV_KEY;
    ev[0].code = static_cast<unsigned short>(code);
    ev[0].value = 1;
    ev[1].type = EV_SYN;
    ev[1].code = SYN_REPORT;
    ev[1].value = 0;
    ev[2].type = EV_KEY;
    ev[2].code = static_cast<unsigned short>(code);
    ev[2].value = 0;
    ev[3].type = EV_SYN;
    ev[3].code = SYN_REPORT;
    ev[3].value = 0;
    ssize_t ignored = write(fd_.get(), ev, sizeof(ev));
    (void)ignored;
  }

  // Batch N backspaces: BACKSPACE_BATCH_SIZE taps per write(), poll()
  // pace between batches.  poll() replaces usleep() so pacing is
  // interruptible; paceFd is watched for readability between batches
  // (reserved for a future cancel protocol — messages queued
  // mid-deletion are still processed only after the batch completes).
  void backspaces(int count, int paceFd) const {
    std::vector<input_event> evs;
    evs.reserve(static_cast<size_t>(count) * 4);
    for (int i = 0; i < count; ++i) {
      input_event press{};
      press.type = EV_KEY;
      press.code = KEY_BACKSPACE;
      press.value = 1;
      input_event release = press;
      release.value = 0;
      input_event syn{};
      syn.type = EV_SYN;
      syn.code = SYN_REPORT;
      evs.push_back(press);
      evs.push_back(syn);
      evs.push_back(release);
      evs.push_back(syn);
    }
    constexpr size_t batchEvents =
        static_cast<size_t>(BACKSPACE_BATCH_SIZE) * 4;
    for (size_t off = 0; off < evs.size(); off += batchEvents) {
      size_t n = std::min(batchEvents, evs.size() - off);
      ssize_t ignored =
          write(fd_.get(), evs.data() + off, n * sizeof(input_event));
      (void)ignored;
      if (off + n >= evs.size()) {
        break;
      }
      pollfd pfd{paceFd, POLLIN, 0};
      poll(&pfd, 1, BATCH_GAP_MS);
    }
  }

  void escape() const { tap(KEY_ESC); }

private:
  Fd fd_;
};

} // namespace

int main(int argc, char **argv) {
  const char *envUser = getenv("SKEY_UINPUT_USER");
  const char *sudoUser = getenv("SUDO_USER");
  std::string targetUser;
  if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
    targetUser = argv[1];
  } else if (envUser != nullptr && envUser[0] != '\0') {
    targetUser = envUser;
  } else if (sudoUser != nullptr && sudoUser[0] != '\0') {
    targetUser = sudoUser;
  } else {
    targetUser = currentUsername();
  }

  passwd targetPwd{};
  std::vector<char> userBuf;
  passwd *target = lookupUser(targetUser, userBuf, targetPwd);
  if (!target) {
    std::cerr << "Cannot resolve user: " << targetUser << "\n";
    return 1;
  }

  UinputDevice uinput;
  if (!uinput.init()) {
    return 1;
  }

  Fd server(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0));
  if (!server) {
    std::cerr << "socket failed: " << strerror(errno) << "\n";
    return 1;
  }

  std::string path = socketPath(targetUser);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  memcpy(&addr.sun_path[1], path.c_str(), path.size());
  socklen_t len = offsetof(sockaddr_un, sun_path) + path.size() + 1;
  if (bind(server.get(), reinterpret_cast<sockaddr *>(&addr), len) != 0) {
    std::cerr << "bind failed: " << strerror(errno) << "\n";
    return 1;
  }
  if (listen(server.get(), 4) != 0) {
    std::cerr << "listen failed: " << strerror(errno) << "\n";
    return 1;
  }

  signal(SIGTERM, onSignal);
  signal(SIGINT, onSignal);
  std::cerr << "SKey uinput server listening for " << targetUser << "\n";

  Fd client;
  while (running.load()) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(server.get(), &readfds);
    int maxFd = server.get();
    if (client) {
      FD_SET(client.get(), &readfds);
      maxFd = std::max(maxFd, client.get());
    }
    timeval timeout{1, 0};
    int ready = select(maxFd + 1, &readfds, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      continue;
    }

    if (FD_ISSET(server.get(), &readfds)) {
      Fd newClient(accept4(server.get(), nullptr, nullptr, SOCK_NONBLOCK));
      if (newClient) {
        ucred cred{};
        socklen_t credLen = sizeof(cred);
        bool ok = getsockopt(newClient.get(), SOL_SOCKET, SO_PEERCRED, &cred,
                             &credLen) == 0 &&
                  cred.uid == target->pw_uid && executableIsFcitx5(cred.pid);
        if (ok) {
          client.reset(newClient.release());
        } else {
          std::cerr << "Rejected unauthorized client\n";
        }
      }
    }

    if (client && FD_ISSET(client.get(), &readfds)) {
      char buf[4096];
      ssize_t n = recv(client.get(), buf, sizeof(buf), 0);
      if (n <= 0) {
        client.reset();
        continue;
      }

      // Protocol:
      //   v1 (8+ bytes):  int32_t count + uint32_t textLen + text
      //   v2 (12+ bytes): int32_t count + uint32_t flags  + uint32_t textLen + text
      //   flags bit 0: send Escape before BS (dismisses Chrome autocomplete)
      // Text is deprecated — replacement text used to be typed via
      // Ctrl+Shift+U hex, but the engine now commits through
      // ic_->commitString().  It is parsed for backward compatibility
      // and never typed.
      int32_t count = 0;
      uint32_t flags = 0;
      if (n == static_cast<ssize_t>(sizeof(int32_t))) {
        memcpy(&count, buf, sizeof(count));
      } else if (n >= static_cast<ssize_t>(sizeof(int32_t) +
                                           sizeof(uint32_t) +
                                           sizeof(uint32_t))) {
        // v2: count + flags + textLen + text
        memcpy(&count, buf, sizeof(count));
        memcpy(&flags, buf + sizeof(count), sizeof(flags));
      } else if (n >= static_cast<ssize_t>(sizeof(int32_t) +
                                           sizeof(uint32_t))) {
        // v1: count + textLen + text (backward-compatible)
        memcpy(&count, buf, sizeof(count));
      } else {
        continue;
      }

      // Flags: bit 0 = send Escape to dismiss autocomplete before BS
      if ((flags & 1) != 0) {
        uinput.escape();
        poll(nullptr, 0, BATCH_GAP_MS);
      }

      count = std::clamp(count, 1, 64);
      uinput.backspaces(count, client.get());
    }
  }

  return 0;
}
