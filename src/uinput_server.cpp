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
// Pacing gap between backspaces (ms).  One BS = press+SYN+release+SYN in
// a single write(), then poll(BACKSPACE_GAP_MS) before the next BS.
// The gap is load-bearing: on Wayland the commit travels via text-input
// while BS travel via uinput — two paths with no shared ordering, so the
// app needs headroom to process each deletion before the sync-anchor BS
// loops back.  Batching multiple BS into one write() with zero gap made
// the anchor return in ~1ms while Electron still had the deletions
// queued, corrupting text ("chào" → "cho", verified on antigravity,
// Wayland, 2026-08-18).  poll() replaces usleep() so pacing stays
// interruptible; paceFd is watched for readability (reserved for a
// future cancel protocol).
constexpr int BACKSPACE_GAP_MS = 1;

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

// Legacy abstract socket name ("skeysocket-<user>-kb_socket").  Abstract
// sockets live in a shared namespace any process can bind, so this is only a
// fallback for old clients; the primary path is the filesystem socket under
// /run (see fsSocketDir) whose parent dir is 0700 user-owned and squat-proof.
std::string abstractSocketName(const std::string &username) {
  std::string path = "skeysocket-" + username + "-kb_socket";
  constexpr size_t maxAbstractSocketName =
      sizeof(((sockaddr_un *)0)->sun_path) - 1;
  if (path.size() > maxAbstractSocketName) {
    path.resize(maxAbstractSocketName);
  }
  return path;
}

std::string fsSocketDir(const std::string &username) {
  return "/run/skey-uinput-" + username;
}

std::string fsSocketPath(const std::string &username) {
  return fsSocketDir(username) + "/kb_socket";
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

// Validate the filesystem socket dir for the unprivileged model: the
// server runs as the skey_uinput sysuser and cannot create or chown
// directories under /run, so the dir must already exist and be writable —
// systemd creates it via RuntimeDirectory=skey-uinput-%i (root:skey_uinput,
// mode 0770).  A root-run (legacy / manual) instance creates it directly.
// Symlink-safe by design: lstat only, never follow.  Returns false if the
// fs socket must be skipped — the server then falls back to the abstract
// socket only.
bool setupFsSocketDir(const std::string &dir) {
  struct stat st{};
  if (lstat(dir.c_str(), &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      // Not a directory (stale file from tampering): unlink, never open.
      if (unlink(dir.c_str()) != 0) {
        std::cerr << "fs socket dir " << dir
                  << " is not a directory and cannot be removed: "
                  << strerror(errno)
                  << " — continuing with abstract socket only\n";
        return false;
      }
    }
  } else if (errno != ENOENT) {
    std::cerr << "fs socket dir stat failed: " << strerror(errno)
              << " — continuing with abstract socket only\n";
    return false;
  }
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    std::cerr << "fs socket dir create failed: " << strerror(errno)
              << " — continuing with abstract socket only\n";
    return false;
  }
  if (geteuid() != 0 && access(dir.c_str(), W_OK) != 0) {
    std::cerr << "fs socket dir not writable (missing RuntimeDirectory?): "
              << strerror(errno)
              << " — continuing with abstract socket only\n";
    return false;
  }
  return true;
}

// Bind the filesystem socket.  Never fatal: on any failure the fd is reset so
// the accept loop skips it and the server continues abstract-only.  The stale
// socket file from a crashed run is unlinked first (unlink only, never open —
// no symlink following).
void bindFsSocket(Fd &fsServer, const std::string &path, uid_t uid, gid_t gid) {
  if (path.size() >= sizeof(((sockaddr_un *)0)->sun_path)) {
    std::cerr << "fs socket path too long — continuing with abstract socket "
                 "only\n";
    fsServer.reset();
    return;
  }
  if (unlink(path.c_str()) != 0 && errno != ENOENT) {
    std::cerr << "failed to remove stale socket file " << path << ": "
              << strerror(errno) << "\n";
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path.c_str(), path.size());
  socklen_t len = offsetof(sockaddr_un, sun_path) + path.size() + 1;
  if (bind(fsServer.get(), reinterpret_cast<sockaddr *>(&addr), len) != 0) {
    std::cerr << "fs socket bind failed: " << strerror(errno)
              << " — continuing with abstract socket only\n";
    fsServer.reset();
    return;
  }
  // connect() requires write permission on the socket file.  The socket is
  // created by the skey_uinput sysuser, so its owner cannot be chowned;
  // chmod 0660 grants group write, and the target user reaches it via
  // membership in the skey_uinput group (added by postinst).  A legacy
  // root run chowns to the target user and keeps 0600 semantics.
  if (geteuid() == 0) {
    if (chown(path.c_str(), uid, gid) != 0 || chmod(path.c_str(), 0600) != 0) {
      std::cerr << "fs socket chown/chmod failed: " << strerror(errno)
                << " — continuing with abstract socket only\n";
      fsServer.reset();
      return;
    }
  } else if (chmod(path.c_str(), 0660) != 0) {
    std::cerr << "fs socket chmod failed: " << strerror(errno)
              << " — continuing with abstract socket only\n";
    fsServer.reset();
    return;
  }
  if (listen(fsServer.get(), 4) != 0) {
    std::cerr << "fs socket listen failed: " << strerror(errno)
              << " — continuing with abstract socket only\n";
    fsServer.reset();
    return;
  }
  std::cerr << "SKey uinput fs socket ready at " << path << "\n";
}

// Legacy abstract socket for old clients.  NON-FATAL: a pre-bound name
// (another instance, or an attacker squatting the predictable name) no
// longer kills the server via Restart=on-failure — new clients use the fs
// socket instead.
void bindAbstractSocket(Fd &server, const std::string &username) {
  std::string path = abstractSocketName(username);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  memcpy(&addr.sun_path[1], path.c_str(), path.size());
  socklen_t len = offsetof(sockaddr_un, sun_path) + path.size() + 1;
  if (bind(server.get(), reinterpret_cast<sockaddr *>(&addr), len) != 0) {
    std::cerr << "abstract socket bind failed (continuing with fs socket "
                 "only): " << strerror(errno) << "\n";
    server.reset();
    return;
  }
  if (listen(server.get(), 4) != 0) {
    std::cerr << "abstract socket listen failed (continuing with fs socket "
                 "only): " << strerror(errno) << "\n";
    server.reset();
  }
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

  // N backspaces, one write() each (press+SYN+release+SYN), poll() gap
  // between them — see BACKSPACE_GAP_MS for why the gap is load-bearing.
  void backspaces(int count, int paceFd) const {
    for (int i = 0; i < count; ++i) {
      input_event ev[4]{};
      ev[0].type = EV_KEY;
      ev[0].code = KEY_BACKSPACE;
      ev[0].value = 1;
      ev[1].type = EV_SYN;
      ev[1].code = SYN_REPORT;
      ev[1].value = 0;
      ev[2].type = EV_KEY;
      ev[2].code = KEY_BACKSPACE;
      ev[2].value = 0;
      ev[3].type = EV_SYN;
      ev[3].code = SYN_REPORT;
      ev[3].value = 0;
      ssize_t ignored = write(fd_.get(), ev, sizeof(ev));
      (void)ignored;
      if (i + 1 < count) {
        pollfd pfd{paceFd, POLLIN, 0};
        poll(&pfd, 1, BACKSPACE_GAP_MS);
      }
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

  // Defense in depth: targetUser feeds /run path construction below.
  if (targetUser.find('/') != std::string::npos) {
    std::cerr << "Invalid target user: " << targetUser << "\n";
    return 1;
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

  // All privileged work (uinput open) happens before anything else.  The
  // process normally runs as the skey_uinput sysuser from the start (the
  // systemd unit sets User= and grants /dev/uinput via udev ACL), so there
  // is no privilege to drop.  A manual root run still works: the fs socket
  // path below detects euid 0 and uses the legacy chown semantics.
  Fd fsServer(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0));
  if (!fsServer) {
    std::cerr << "socket failed: " << strerror(errno) << "\n";
    return 1;
  }
  if (setupFsSocketDir(fsSocketDir(targetUser))) {
    bindFsSocket(fsServer, fsSocketPath(targetUser), target->pw_uid,
                 target->pw_gid);
  }

  Fd abstractServer(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0));
  if (abstractServer) {
    bindAbstractSocket(abstractServer, targetUser);
  } else {
    std::cerr << "abstract socket creation failed: " << strerror(errno)
              << " — continuing with fs socket only\n";
  }

  signal(SIGTERM, onSignal);
  signal(SIGINT, onSignal);
  std::cerr << "SKey uinput server listening for " << targetUser << "\n";

  Fd client;
  Fd *listeners[] = {&fsServer, &abstractServer};
  while (running.load()) {
    fd_set readfds;
    FD_ZERO(&readfds);
    int maxFd = -1;
    for (Fd *s : listeners) {
      // Never FD_SET(-1): a negative fd corrupts the fd_set bit array.
      if (s->get() >= 0) {
        FD_SET(s->get(), &readfds);
        maxFd = std::max(maxFd, s->get());
      }
    }
    if (client) {
      FD_SET(client.get(), &readfds);
      maxFd = std::max(maxFd, client.get());
    }
    if (maxFd < 0) {
      // Both binds failed: stay alive and signal-terminable, no crash loop.
      poll(nullptr, 0, 1000);
      continue;
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

    for (Fd *s : listeners) {
      if (s->get() < 0 || !FD_ISSET(s->get(), &readfds)) {
        continue;
      }
      Fd newClient(accept4(s->get(), nullptr, nullptr, SOCK_NONBLOCK));
      if (!newClient) {
        continue; // EINTR etc.: skip this round
      }
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
        poll(nullptr, 0, BACKSPACE_GAP_MS);
      }

      count = std::clamp(count, 1, 64);
      uinput.backspaces(count, client.get());
    }
  }

  return 0;
}
