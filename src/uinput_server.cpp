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

namespace {

// Timing tunables (microseconds) — adjust if input is lost or laggy
constexpr useconds_t UINPUT_INIT_WAIT_US = 1000000;
constexpr useconds_t KEY_EVENT_DELAY_US = 500;
constexpr useconds_t UNICODE_COMPOSE_US = 2000;
constexpr useconds_t BACKSPACE_GAP_US = 500;
constexpr useconds_t BACKSPACE_SETTLE_US = 500;

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
    const int keys[] = {
        KEY_BACKSPACE, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_U, KEY_ENTER, KEY_0,
        KEY_1,         KEY_2,        KEY_3,         KEY_4, KEY_5,     KEY_6,
        KEY_7,         KEY_8,        KEY_9,         KEY_A, KEY_B,     KEY_C,
        KEY_D,         KEY_E,        KEY_F};
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

  void key(int code, int value) const {
    input_event ev[2]{};
    ev[0].type = EV_KEY;
    ev[0].code = code;
    ev[0].value = value;
    ssize_t ignored = write(fd_.get(), ev, sizeof(ev));
    (void)ignored;
    usleep(KEY_EVENT_DELAY_US);
  }

  void tap(int code) const {
    key(code, 1);
    key(code, 0);
  }

  void backspace() const { tap(KEY_BACKSPACE); }

  int hexKey(char ch) const {
    if (ch >= '0' && ch <= '9') {
      return KEY_0 + (ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
      return KEY_A + (ch - 'a');
    }
    return KEY_0;
  }

  void typeCodepoint(uint32_t cp) const {
    char hex[16];
    snprintf(hex, sizeof(hex), "%x", cp);
    key(KEY_LEFTCTRL, 1);
    key(KEY_LEFTSHIFT, 1);
    tap(KEY_U);
    key(KEY_LEFTSHIFT, 0);
    key(KEY_LEFTCTRL, 0);
    usleep(UNICODE_COMPOSE_US);
    for (const char *p = hex; *p != '\0'; ++p) {
      tap(hexKey(*p));
    }
    tap(KEY_ENTER);
  }

  void typeUtf8(const std::string &text) const {
    for (size_t i = 0; i < text.size();) {
      unsigned char ch = static_cast<unsigned char>(text[i]);
      uint32_t cp = 0;
      size_t advance = 1;
      if (ch < 0x80) {
        cp = ch;
      } else if ((ch & 0xE0) == 0xC0 && i + 1 < text.size()) {
        cp = ((ch & 0x1F) << 6) |
             (static_cast<unsigned char>(text[i + 1]) & 0x3F);
        advance = 2;
      } else if ((ch & 0xF0) == 0xE0 && i + 2 < text.size()) {
        cp = ((ch & 0x0F) << 12) |
             ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
             (static_cast<unsigned char>(text[i + 2]) & 0x3F);
        advance = 3;
      } else if ((ch & 0xF8) == 0xF0 && i + 3 < text.size()) {
        cp = ((ch & 0x07) << 18) |
             ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
             ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
             (static_cast<unsigned char>(text[i + 3]) & 0x3F);
        advance = 4;
      } else {
        cp = ch;
      }
      typeCodepoint(cp);
      i += advance;
    }
  }

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

      int32_t count = 0;
      std::string text;
      if (n == static_cast<ssize_t>(sizeof(int32_t))) {
        memcpy(&count, buf, sizeof(count));
      } else if (n >=
                 static_cast<ssize_t>(sizeof(int32_t) + sizeof(uint32_t))) {
        uint32_t textLen = 0;
        memcpy(&count, buf, sizeof(count));
        memcpy(&textLen, buf + sizeof(count), sizeof(textLen));
        size_t available =
            static_cast<size_t>(n) - sizeof(count) - sizeof(textLen);
        textLen = std::min<uint32_t>(textLen, static_cast<uint32_t>(available));
        text.assign(buf + sizeof(count) + sizeof(textLen),
                    buf + sizeof(count) + sizeof(textLen) + textLen);
      } else {
        continue;
      }

      count = std::clamp(count, 1, 64);
      for (int i = 0; i < count; ++i) {
        uinput.backspace();
        usleep(BACKSPACE_GAP_US);
      }
      if (!text.empty()) {
        usleep(BACKSPACE_SETTLE_US);
        uinput.typeUtf8(text);
      }
    }
  }

  return 0;
}
